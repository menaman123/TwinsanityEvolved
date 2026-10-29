#!/usr/bin/env python3
"""language_table_judge.py -- does the game offer EVERY language the disc carries, as two independent rows: the TEXT
languages and the SPEECH languages?

The PAL image used for development is a text-only patch: the `English` slot's text is Portuguese and the `Spanish` slot's text is English, every
speech bank (Crash6\\<Slot>.mb), FMV track and title card is the 2004 retail file. The text and the speech are two settings, so
the table is two lists. This judge checks, against the disc itself:

  A  EVIDENCE   ten strings per slot off the disc (Language\\Code\\<slot>.txt lines 9-15: the title menu and the Options rows;
                Language\\AgentLab\\<slot>.txt lines 0, 4, 7: the credits' thank-you and the first level's dialogue) and the
                language they are in, decided by a per-language word table over all ten (not the one "new game" string). The
                table is printed; the fixture's per-slot verdict must agree with it (the fixture is frozen FROM this evidence).
  B  TABLE      a run's language line lists the TEXT languages (one per text content, english first, then slot order; then the
                language packs the fixture names) and the SPEECH languages (one per speech bank, english first, then slot
                order): on this disc text english portuguese french german italian, speech english french german spanish
                italian -- spanish is a speech language, portuguese a text language, neither needs the other.
  C  SELECTION  one deterministic headless run per pairing of the fixture ("pairs": every text language and every speech
                language at least once, each with ANOTHER language for the other row) with `--text-language X --speech-language
                Y` to the Title: both chosen BY NAME, the Code text the game ended in (`game text 9`) is X's line 9, the speech
                bank opened is Y's Crash6\\<Slot>.mb, the voice slot is Y's, GameController reached Title (7).
  D  MENU       one run playing fixtures/padlogs/language_rows.padlog (title -> OPTIONS -> GAME OPTIONS -> the TEXT LANGUAGE row
                stepped RIGHT through every choice, then the SPEECH LANGUAGE row through every choice): each row cycles through
                every language of its list, a text step leaves the speech as it was, a speech step leaves the text as it was,
                and every speech step opens that language's Crash6\\<Slot>.mb.

RED on bin/twinsanityevolved.310b6f0 (no spanish row: C's spanish run falls back to english, D cycles five rows), GREEN after.
2026-10-27: two lists; RED on the runner of 43e9989 (one row of tuples, no --text-language), GREEN on its change.

A fixture may name language packs in the mods folder for every run ("runner_args") and the text languages they add after
the disc's ("mod_rows": fixtures/judges/language_table_arabic_pal.json): the text list must then end with them, the menu's
text row must step through them too, and the speech list is unchanged (a pack never adds speech).

Usage: language_table_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--evidence-only]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / the disc has no banks.
"""
import argparse
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "language_table_pal.json")

PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
CODE_LINES = [9, 10, 11, 12, 13, 14, 15]          # new game, load game, options, language, graphic / sound / game options
AGENT_LINES = [0, 4, 7]                           # thanks for playing; "you gotta help me crash"; "what did i do to deserve this"

# The word table: a string is in a language when it starts with one of that language's words (lower case; accents as the disc
# writes them in Latin-1, matched on their ASCII prefix where they differ)
WORDS = {
    "english": ["new game", "load game", "options", "language", "graphic options", "sound options", "game options",
                "thanks for playing", "you gotta help me", "what did i do"],
    "portuguese": ["novo jogo", "carregar jogo", "op\xe7\xf5es", "idioma", "op\xe7\xf5es gr", "op\xe7\xf5es de som", "op\xe7\xf5es do jogo",
                   "obrigado por jogar", "voc\xea tem que me ajudar", "o que eu fiz"],
    "french": ["nouvelle partie", "charger", "options", "langue", "options graphiques", "options sonores", "options de jeu",
               "merci d'avoir jou", "tu dois m'aider", "qu'est-ce que j'ai fait"],
    "german": ["neues spiel", "spiel laden", "optionen", "sprache", "grafik-optionen", "sound-optionen", "spiel-optionen",
               "danke f\xfcrs spielen", "du musst mir helfen", "womit hab ich das verdient"],
    "italian": ["nuova partita", "carica partita", "opzioni", "lingua", "opzioni grafiche", "opzioni audio", "opzioni di gioco",
                "grazie per aver giocato", "devi aiutarmi", "cosa ho fatto"],
    "spanish": ["nuevo juego", "nueva partida", "juego nuevo", "cargar partida", "cargar juego", "opciones", "idioma", "opciones gr",
                "opciones de sonido", "opciones de juego", "gracias por jugar", "tienes que ayudarme", "qu\xe9 he hecho", "qu\xe9 hice"],
}

LANGLINE = re.compile(r"\[twinsanityevolved\] language: (.*)")
TEXTS = re.compile(r"; text languages((?: \[[^\]]*\])*)")
SPEECHES = re.compile(r"; speech languages((?: \[[^\]]*\])*)")
TEXT_ROW = re.compile(r"\[(\w+): text slot (\d+)(?:, pack (\w+))?\]")
SPEECH_ROW = re.compile(r"\[(\w+): voice slot (\d+) \(([^)]*)\)\]")
SELECTED_BY = re.compile(r"requested text '([^']*)' speech '([^']*)' -> text (\w+) by ([^:]+): text slot (\d+); speech (\w+) by ([^:]+): voice slot (\d+); Language\(\) (\d+)")
TEXT9 = re.compile(r"game text 9 = '([^']*)'")
BANK_OPEN = re.compile(r'\[stream\] open #(\d+) "Crash6\\(\w+)\.mb" -> found')
GSTATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")


# ---- the disc ------------------------------------------------------------------------------------------------------------
def iso_dir(f, lba, size):
    f.seek(lba * 2048)
    d = f.read(size)
    out, off = [], 0
    while off < len(d):
        ln = d[off]
        if ln == 0:
            off = (off // 2048 + 1) * 2048
            continue
        e_lba, e_size = struct.unpack_from("<I", d, off + 2)[0], struct.unpack_from("<I", d, off + 10)[0]
        nl = d[off + 32]
        name = d[off + 33:off + 33 + nl].decode("latin1").split(";")[0]
        out.append((name, e_lba, e_size, d[off + 25] & 2))
        off += ln
    return out


def text_lines(b):
    return [l for l in re.split(r"[\r\n]+", b.decode("latin1").lower()) if l.strip()]


def language_of(strings):
    """(language, hits) by the word table over the strings: the most hits wins, at least half of them."""
    best, best_hits = None, 0
    for lang, words in WORDS.items():
        hits = sum(1 for s in strings if any(s.startswith(w) for w in words))
        if hits > best_hits:
            best, best_hits = lang, hits
    return (best if best_hits * 2 >= len(strings) else None), best_hits


def disc_slots(iso):
    """{slot: {name, strings (10), text (the language of the ten), hits, text9, speech, speech_bank}} per PAL slot with a Code bank."""
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        crash6 = [e for e in iso_dir(f, root_lba, root_size) if e[0].lower() == "crash6" and e[3]]
        if not crash6:
            return {}
        entries = {e[0].upper(): e for e in iso_dir(f, crash6[0][1], crash6[0][2])}
        if "CRASH.BH" not in entries or "CRASH.BD" not in entries:
            return {}
        f.seek(entries["CRASH.BH"][1] * 2048)
        bh = f.read(entries["CRASH.BH"][2])
        index, off = {}, 4
        while off + 4 <= len(bh):
            nl = struct.unpack_from("<I", bh, off)[0]
            off += 4
            if nl > 512 or off + nl + 8 > len(bh):
                break
            name = bh[off:off + nl].decode("latin1")
            off += nl
            o, s = struct.unpack_from("<II", bh, off)
            off += 8
            index[name] = (o, s)
        base = entries["CRASH.BD"][1] * 2048

        def read(name):
            if name not in index:
                return b""
            o, s = index[name]
            f.seek(base + o)
            return f.read(s)

        slots = {}
        for slot, name in enumerate(PAL_SLOTS):
            code = text_lines(read("Language\\Code\\%s.txt" % name))
            if not code:
                continue
            agent = [l for l in text_lines(read("Language\\AgentLab\\%s.txt" % name)) if l.strip() != "zzz"]
            strings = [code[i] if i < len(code) else "" for i in CODE_LINES] + [agent[i] if i < len(agent) else "" for i in AGENT_LINES]
            text, hits = language_of(strings)
            mb = ("%s.MB" % name).upper()
            speech = mb in entries and entries[mb][2] > 0
            slots[slot] = {"name": name, "strings": strings, "text": text or name.lower(), "recognised": text is not None, "hits": hits,
                           "text9": code[9] if len(code) > 9 else "", "speech": name.lower() if speech else None,
                           "speech_bank": "Crash6\\%s.mb" % name if speech else None}
        return slots


def derive_lists(slots):
    """The expected two lists: texts {language: {text_slot}} (one per text content, english first, then slot order) and
    speeches {language: {voice_slot, speech_bank}} (one per speech bank, english first, then slot order)."""
    texts, speeches = {}, {}
    for slot in sorted(slots):
        t = slots[slot]["text"]
        if t not in texts:
            texts[t] = {"text_slot": slot, "text9": slots[slot]["text9"]}
        sp = slots[slot]["speech"]
        if sp and sp not in speeches:
            speeches[sp] = {"voice_slot": slot, "speech_bank": slots[slot]["speech_bank"]}
    order = lambda d: {k: d[k] for k in (["english"] if "english" in d else []) + [k for k in d if k != "english"]}
    return order(texts), order(speeches)


# ---- the runs ------------------------------------------------------------------------------------------------------------
def supports(binary, flag):
    """Does the runner's usage (--help, headless, no stdin) name `flag`?"""
    try:
        r = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, TE_GFX_HEADLESS="1"))
    except Exception:
        return False
    return flag in (r.stderr + r.stdout)


RUNNER_ARGS = []   # the fixture's "runner_args" (every run): a language mod switched on


def run(binary, iso, out, tag, extra, stop_at, run_seconds):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--trace-state", "--mc-dir", mc,
            "--stop-at", str(stop_at), "--run-seconds", str(run_seconds)] + RUNNER_ARGS + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_STREAM_TRACE"] = "1"
    print("language_table_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("language_table_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return log, rc


def parse_log(path):
    """The language lines in order (the two lists, the selection each announces), the speech banks opened in order (with the
    line number), the game's text 9 at exit, whether Title (7) was reached."""
    r = {"lines": [], "banks": [], "text9": None, "title": False, "texts": None, "speeches": None}
    with open(path, encoding="latin-1") as f:   # byte for byte (a language pack's text is Windows-1256)
        for n, line in enumerate(f):
            m = LANGLINE.search(line)
            if m:
                body = m.group(1)
                mt, ms = TEXTS.search(body), SPEECHES.search(body)
                texts = [(x[0], int(x[1]), x[2]) for x in TEXT_ROW.findall(mt.group(1))] if mt else []
                speeches = [(x[0], int(x[1]), x[2]) for x in SPEECH_ROW.findall(ms.group(1))] if ms else []
                sel = SELECTED_BY.search(body)
                entry = {"n": n, "texts": [x[0] for x in texts], "speeches": [x[0] for x in speeches], "selected": None,
                         "text_slots": {x[0]: x[1] for x in texts}, "packs": {x[0]: x[2] for x in texts if x[2]},
                         "voice_slots": {x[0]: x[1] for x in speeches}}
                if sel:
                    entry["selected"] = {"requested_text": sel.group(1), "requested_speech": sel.group(2), "text": sel.group(3),
                                         "how_text": sel.group(4), "text_slot": int(sel.group(5)), "speech": sel.group(6),
                                         "how_speech": sel.group(7), "voice_slot": int(sel.group(8)), "code": int(sel.group(9))}
                r["lines"].append(entry)
                if r["texts"] is None and (texts or speeches):
                    r["texts"], r["speeches"] = entry["texts"], entry["speeches"]
                m9 = TEXT9.search(body)
                if m9:
                    r["text9"] = m9.group(1)
            m = BANK_OPEN.search(line)
            if m and m.group(2).lower() != "music":
                r["banks"].append((n, m.group(2)))
            m = GSTATE.search(line)
            if m and int(m.group(3)) == 7:
                r["title"] = True
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/language_table_judge)")
    ap.add_argument("--evidence-only", action="store_true", help="print the ten-strings-per-slot table and the derived table; no runs")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "language_table_judge")
    os.makedirs(out, exist_ok=True)

    # A -- the evidence
    slots = disc_slots(a.iso)
    if not slots:
        print("language_table_judge: the disc carries no Language\\Code banks the judge can read", file=sys.stderr)
        return 2
    fails = []
    print("language_table_judge: ten strings per slot (Code lines %s; AgentLab lines %s)" % (CODE_LINES, AGENT_LINES))
    for slot in sorted(slots):
        s = slots[slot]
        print("  slot %d %-8s text=%s (%d/10 words) speech=%s" % (slot, s["name"], s["text"], s["hits"], s["speech"]))
        for i, st in enumerate(s["strings"]):
            print("      %s %s" % ("C%d" % CODE_LINES[i] if i < len(CODE_LINES) else "A%d" % AGENT_LINES[i - len(CODE_LINES)], st[:70]))
    fx_slots = {int(k): v for k, v in fx["slots"].items()}
    same_disc = len(slots) == len(fx_slots) and all(k in slots and slots[k]["text"] == v["text"] and slots[k]["speech"] == v["speech"] for k, v in fx_slots.items())
    if same_disc:
        texts, speeches = dict(fx["texts"]), dict(fx["speeches"])
        print("language_table_judge: the disc's slots match the frozen fixture (%s): its lists stand" % os.path.relpath(a.fixture, ROOT))
    else:
        texts, speeches = derive_lists(slots)
        print("language_table_judge: another disc than the fixture's: the lists are derived from its slots")
    # a fixture with language packs in the mods folder (runner_args) names the text languages they add after the disc's
    global RUNNER_ARGS
    RUNNER_ARGS = list(fx.get("runner_args", []))
    for lang, row in fx.get("mod_rows", {}).items():
        texts[lang] = row
    texts_expected, speeches_expected = list(texts), list(speeches)
    print(json.dumps({"texts": texts, "speeches": speeches}))
    if "spanish" not in speeches:
        print("language_table_judge: NOTE the disc has no Spanish speech bank: no spanish speech language is expected")
    if a.evidence_only:
        return 0
    if not supports(a.bin, "--text-language"):
        print(json.dumps({"verdict": "red", "bin": a.bin, "reasons": ["the runner has no --text-language / --speech-language: one language row "
                                                                     "drives the text and the speech (no two lists)"]}))
        print("language_table_judge: RED (the runner has no text and speech languages of their own)")
        return 1

    # B + C -- one run per pairing (text X, speech Y)
    pairs = fx.get("pairs") or [[t, speeches_expected[i % len(speeches_expected)]] for i, t in enumerate(texts_expected)]
    covered_t = {t for t, _ in pairs}
    covered_s = {sp for _, sp in pairs}
    verdicts = {}
    for lang in [l for l in texts_expected if l not in covered_t]:
        fails.append("C: the pairs leave the text language %s out" % lang)
    for lang in [l for l in speeches_expected if l not in covered_s]:
        fails.append("C: the pairs leave the speech language %s out" % lang)
    for t, sp in pairs:
        tag = "%s+%s" % (t, sp)
        if t not in texts or sp not in speeches:
            fails.append("%s: the disc has no such pair (texts %s, speeches %s)" % (tag, texts_expected, speeches_expected))
            continue
        log, rc = run(a.bin, a.iso, out, tag, ["--text-language", t, "--speech-language", sp], fx.get("stop_at", 2500), fx.get("run_seconds", 300))
        if rc != 0:
            print("language_table_judge: run %s exited %d" % (tag, rc), file=sys.stderr)
            return 2
        r = parse_log(log)
        f = []
        if r["texts"] is None:
            f.append("TABLE: the runner printed no text / speech lists (one row of language tuples?)")
        else:
            if r["texts"] != texts_expected:
                f.append("TABLE: text languages %s, expected %s" % (r["texts"], texts_expected))
            if r["speeches"] != speeches_expected:
                f.append("TABLE: speech languages %s, expected %s" % (r["speeches"], speeches_expected))
        sel = r["lines"][0]["selected"] if r["lines"] else None
        if sel is None:
            f.append("SELECTION: the runner reported no text / speech selection")
        else:
            if sel["how_text"] != "name" or sel["text"] != t:
                f.append("SELECTION: the text is %s by %s, not %s by name" % (sel["text"], sel["how_text"], t))
            if sel["how_speech"] != "name" or sel["speech"] != sp:
                f.append("SELECTION: the speech is %s by %s, not %s by name" % (sel["speech"], sel["how_speech"], sp))
            if sel["text_slot"] != texts[t]["text_slot"] or sel["voice_slot"] != speeches[sp]["voice_slot"]:
                f.append("SELECTION: text slot %d / voice slot %d, expected %d / %d" % (sel["text_slot"], sel["voice_slot"], texts[t]["text_slot"],
                                                                                        speeches[sp]["voice_slot"]))
        bank = "Crash6\\%s.mb" % r["banks"][-1][1] if r["banks"] else None
        if (bank or "").lower() != (speeches[sp]["speech_bank"] or "").lower():
            f.append("SPEECH: the speech bank opened is %s, expected %s" % (bank, speeches[sp]["speech_bank"]))
        # a language pack's text 9 is its bank's (Windows-1256 bytes, as the log carries them); a disc language's the slot's line 9
        want9 = texts[t]["text9"].encode("cp1256").decode("latin-1") if texts[t].get("mod") else slots[texts[t]["text_slot"]]["text9"]
        if r["text9"] != want9:
            f.append("TEXT: game text 9 is %r, expected %r" % (r["text9"], want9))
        if not r["title"]:
            f.append("the run did not reach Title (7)")
        verdicts[tag] = {"texts": r["texts"], "speeches": r["speeches"], "selected": sel, "speech_bank": bank, "text9": r["text9"],
                         "title": r["title"], "verdict": "green" if not f else "red", "reasons": f}
        print(json.dumps({"run": tag, **verdicts[tag]}))
        fails += ["%s: %s" % (tag, x) for x in f]

    # D -- the Options menu steps both rows through every language, each leaving the other alone
    log, rc = run(a.bin, a.iso, out, "menu", ["--skip-movies", "--text-language", fx.get("menu_start", "english"), "--speech-language",
                                               fx.get("menu_start", "english"), "--play-pad", os.path.join(ROOT, fx["menu_padlog"])],
                  fx.get("menu_stop_at", 2400), fx.get("run_seconds", 300))
    if rc != 0:
        print("language_table_judge: the menu run exited %d" % rc, file=sys.stderr)
        return 2
    r = parse_log(log)
    steps = [e for e in r["lines"] if e["selected"] and "options menu" in (e["selected"]["how_text"], e["selected"]["how_speech"])]
    text_steps, speech_steps = [], []
    f = []
    prev = None
    for e in steps:
        sel = e["selected"]
        if prev is not None:
            moved_text, moved_speech = sel["text"] != prev["text"], sel["speech"] != prev["speech"]
            if moved_text and moved_speech:
                f.append("MENU: one step changed both rows (%s/%s -> %s/%s)" % (prev["text"], prev["speech"], sel["text"], sel["speech"]))
            if moved_text:
                text_steps.append((e["n"], sel["text"]))
            if moved_speech:
                speech_steps.append((e["n"], sel["speech"]))
        else:
            # the first step: from the run's start (english / english)
            if sel["text"] != fx.get("menu_start", "english"):
                text_steps.append((e["n"], sel["text"]))
            if sel["speech"] != fx.get("menu_start", "english"):
                speech_steps.append((e["n"], sel["speech"]))
        prev = sel
    if not steps:
        f.append("MENU: no Options-menu language step was logged (the pad log did not reach the rows?)")
    stepped_t = [fx.get("menu_start", "english")] + [x for _, x in text_steps]
    stepped_s = [fx.get("menu_start", "english")] + [x for _, x in speech_steps]
    missing_t = [l for l in texts_expected if l not in stepped_t]
    missing_s = [l for l in speeches_expected if l not in stepped_s]
    if missing_t:
        f.append("MENU: the TEXT LANGUAGE row never showed %s (stepped: %s)" % (missing_t, stepped_t))
    if missing_s:
        f.append("MENU: the SPEECH LANGUAGE row never showed %s (stepped: %s)" % (missing_s, stepped_s))
    for n, lang in speech_steps:
        after = [b for (m, b) in r["banks"] if m > n]
        want = speeches.get(lang, {}).get("speech_bank", "")
        if not after or ("Crash6\\%s.mb" % after[0]).lower() != (want or "").lower():
            f.append("MENU: the speech step to %s did not open %s (banks after it: %s)" % (lang, want, after[:2]))
    verdicts["menu"] = {"text_steps": [x for _, x in text_steps], "speech_steps": [x for _, x in speech_steps], "banks": [b for n, b in r["banks"]],
                        "verdict": "green" if not f else "red", "reasons": f}
    print(json.dumps({"run": "menu", **verdicts["menu"]}))
    fails += ["menu: %s" % x for x in f]

    verdict = "GREEN" if not fails else "RED"
    print(json.dumps({"verdict": verdict.lower(), "bin": a.bin, "pairs": pairs, "texts_expected": texts_expected, "speeches_expected": speeches_expected,
                      "reasons": fails}))
    print("language_table_judge: %s (%s)" % (verdict, ", ".join("%s %s" % (k, v["verdict"]) for k, v in verdicts.items())))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
