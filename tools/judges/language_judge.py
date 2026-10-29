#!/usr/bin/env python3
"""language_judge.py -- does the title show the TEXT LANGUAGE ASKED FOR, whatever the speech language?

The disc is the ground truth. The judge reads the disc's archive index itself (CRASH6/CRASH.BH -> Language\\Code\\<slot>.txt in
CRASH6/CRASH.BD) and recognises each slot's CONTENT from its menu lines ("new game" / "novo jogo" / "nouvelle partie" ...), so it
knows which slot carries the language that is requested -- on the development SLES-52568 image the `English` slot holds Brazilian
Portuguese and the English text sits in the `Spanish` slot.

Three deterministic headless runs of the title to the main menu (fixtures/judges/language_pal.json: START at the title, the main
menu held; frames dumped every 100 presents):

  A  the TEXT language REQUESTED by name with ANOTHER speech language (`--text-language english --speech-language <the
     fixture's speech>`: the text row is a setting of its own; a runner without --text-language is RED: its text cannot
     be chosen apart from the speech)
  B  the slot whose content IS that language, named RAW (`--language-bank <slot>`: text = voice = the slot, as retail)
  C  another text language the disc carries (`other`), by name (`--text-language <other>`, the same speech as A)

and GREEN iff, on the judged present, the main menu's TEXT REGION of A equals B's pixel for pixel (the requested language's
strings are drawn: the same glyphs at the same places at the same breathing phase) and differs from C's (another language draws
other glyphs) -- the text-quad hashes (`quadhash` on the TE_GFX_DIAG_TEXT lines, runners with the language setting) are reported per run and must
agree the same way when present; and, when the runner prints its language line, the text the game ended in (`game text 9`)
is the requested language's "new game" line read off the disc.

RED on bin/twinsanityevolved.32e1174 (`--language 1` = English requested -> the `English` slot -> Portuguese drawn; B via
`--language 3` draws the English text: A != B), GREEN on the language-setting build for english and for a second language (--other).
2026-10-27: A and C name the text row alone, with a speech language other than the text's (the fixture's "speech"):
RED on the runner of 43e9989 (no --text-language), GREEN on its change.

A fixture may name extra runner arguments for every arm ("runner_args": the language mod switched on, so the disc's languages
are judged with a language mod's row in the table: fixtures/judges/language_arabic_pal.json).

Usage: language_judge.py --bin BIN --iso ISO [--language english] [--other french] [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed / the disc has no banks.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "language_pal.json")
QUAD = re.compile(r"\[te\.gfx\.diag\] present (\d+) text (\d+) quads .* quadhash ([0-9a-f]{16})")
TEXT9 = re.compile(r"\[twinsanityevolved\] language: .*; game text 9 = '([^']*)'")
SELECTED = re.compile(r"requested '([^']*)' -> slot (\d+) (\w+) \((\w+)\) by ([^,]+), Language\(\) (\d+)")
# the two rows' selection: "requested text 'x' speech 'y' -> text <t> by <how>: text slot N; speech <s> by <how>: voice slot M; Language() C"
ROWS_SELECTED = re.compile(r"requested text '([^']*)' speech '([^']*)' -> text (\w+) by ([^:]+): text slot (\d+); speech (\w+) by ([^:]+): voice slot (\d+); Language\(\) (\d+)")

# The game's GameLanguageOf (game/src/game/gamecontext.cpp), inverted: the console code that selects a slot of that NAME
CODE_OF_SLOT = {"English": 1, "French": 2, "German": 4, "Spanish": 3, "Italian": 5}
PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
# Line 9 of a Code bank (the main menu's "new game") per language (lower case; the host's RecognizeContent agrees)
NEW_GAME = {"english": "new game", "portuguese": "novo jogo", "french": "nouvelle partie", "german": "neues spiel",
            "italian": "nuova partita", "spanish": ("nuevo juego", "nueva partida", "juego nuevo"), "dutch": "nieuw spel"}


# ---- the disc ------------------------------------------------------------------------------------------------------------
def iso_dir(f, lba, size):
    f.seek(lba * 2048)
    d = f.read(size)
    out = []
    off = 0
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


def disc_banks(iso):
    """[(slot, slot name, content language or None, line 9)] for every PAL slot the disc's archive carries a Code bank for."""
    with open(iso, "rb") as f:
        f.seek(16 * 2048)
        pvd = f.read(2048)
        root_lba, root_size = struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0]
        crash6 = [e for e in iso_dir(f, root_lba, root_size) if e[0] == "CRASH6" and e[3]]
        if not crash6:
            return []
        entries = {e[0]: e for e in iso_dir(f, crash6[0][1], crash6[0][2])}
        if "CRASH.BH" not in entries or "CRASH.BD" not in entries:
            return []
        f.seek(entries["CRASH.BH"][1] * 2048)
        bh = f.read(entries["CRASH.BH"][2])
        index = {}
        off = 4
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
        banks = []
        for slot, name in enumerate(PAL_SLOTS):
            key = "Language\\Code\\%s.txt" % name
            if key not in index:
                continue
            o, s = index[key]
            f.seek(base + o)
            text = f.read(min(s, 2048)).decode("latin1").lower()
            lines = [l for l in re.split(r"[\r\n]+", text) if l]
            line9 = lines[9] if len(lines) > 9 else ""
            content = None
            for lang, words in NEW_GAME.items():
                words = (words,) if isinstance(words, str) else words
                if line9 in words:
                    content = lang
            banks.append((slot, name, content, line9))
        return banks


# ---- the runs -------------------------------------------------------------------------------------------------------------
def run(binary, iso, out, fx, lang_args, tag):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    dump, mc = os.path.join(d, "dump"), os.path.join(d, "mc")
    os.makedirs(dump)
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + fx.get("runner_args", []) + lang_args
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_DUMP_DIR"] = dump
    env["TE_GFX_DUMP_EVERY"] = str(fx["dump_every"])
    env["TE_GFX_DIAG_TEXT"] = "1"
    print("language_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("language_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc


def supports(binary, flag):
    """Does the runner know `flag`? Its usage (--help) names every option. Headless and without stdin: a runner started with
    no disc image may otherwise wait for one to be chosen, and a probe that times out would judge a newer runner by the old
    runner's console codes."""
    try:
        r = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, TE_GFX_HEADLESS="1"))
    except Exception:
        return False
    return flag in (r.stderr + r.stdout)


def read_ppm(path):
    with open(path, "rb") as f:
        data = f.read()
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
    return w, h, memoryview(data)[pos:pos + 3 * w * h]


def region_hash(path, box):
    w, h, px = read_ppm(path)
    x0, y0, x1, y1 = [int(round(v * (w if i % 2 == 0 else h))) for i, v in enumerate(box)]
    m = hashlib.sha256()
    lit = 0
    for y in range(y0, y1):
        row = px[(y * w + x0) * 3:(y * w + x1) * 3]
        m.update(row)
        # the menu's glyphs are orange on the blue orb: red-dominant pixels count as text
        lit += sum(1 for i in range(0, len(row), 3) if row[i] > 140 and row[i] - row[i + 2] > 60)
    return m.hexdigest()[:16], lit


def parse_log(path, present):
    quads, text9, selected = set(), None, None
    with open(path, errors="replace") as f:
        for line in f:
            m = QUAD.search(line)
            if m and int(m.group(1)) == present:
                quads.add(m.group(3))
            m = TEXT9.search(line)
            if m:
                text9 = m.group(1)
            m = SELECTED.search(line)
            if m:
                selected = {"requested": m.group(1), "slot": int(m.group(2)), "name": m.group(3), "content": m.group(4), "how": m.group(5), "code": int(m.group(6))}
            m = ROWS_SELECTED.search(line)
            if m:
                selected = {"requested": m.group(1), "content": m.group(3), "how": m.group(4), "slot": int(m.group(5)), "speech": m.group(6),
                            "speech_how": m.group(7), "voice_slot": int(m.group(8)), "code": int(m.group(9))}
    return quads, text9, selected


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--language", default="english", help="the language requested (default english)")
    ap.add_argument("--other", default="french", help="a second language the disc carries (the C arm)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/language_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "language_judge")
    os.makedirs(out, exist_ok=True)

    banks = disc_banks(a.iso)
    if not banks:
        print("language_judge: the disc carries no Language\\Code banks the judge can read", file=sys.stderr)
        return 2
    for slot, name, content, line9 in banks:
        print(json.dumps({"disc_slot": slot, "name": name, "content": content or "(unrecognised: %s)" % name.lower(), "text9": line9}))
    want = a.language.lower()
    other = a.other.lower()
    by_content = {b[2]: b for b in banks if b[2]}
    by_name = {b[1].lower(): b for b in banks}
    want_bank = by_content.get(want) or by_name.get(want)
    other_bank = by_content.get(other) or by_name.get(other)
    if not want_bank or not other_bank or want_bank[0] == other_bank[0]:
        print("language_judge: the disc has no bank for '%s' and a different one for '%s'" % (want, other), file=sys.stderr)
        return 2
    expected_text9 = want_bank[3]

    if fx.get("text_row") and not supports(a.bin, "--text-language"):
        print(json.dumps({"verdict": "RED", "requested": want, "why": ["the runner has no --text-language: the text cannot be chosen apart from the speech (one language for both)"]}))
        print("language_judge: RED (the runner has no text language of its own: --text-language)")
        return 1
    new_runner = supports(a.bin, "--language-bank")
    print("language_judge: runner %s --language-bank (%s)" % ("supports" if new_runner else "lacks", "language-setting build" if new_runner else "no --language-bank: console codes used"))
    if new_runner and fx.get("text_row"):
        speech = ["--speech-language", fx["text_row"]["speech"]]
        arms = {"A": ["--text-language", want] + speech, "B": ["--language-bank", str(want_bank[0])], "C": ["--text-language", other] + speech}
    elif new_runner:
        arms = {"A": ["--language", want], "B": ["--language-bank", str(want_bank[0])], "C": ["--language", other]}
    else:
        # the runner without --language-bank: a console code; "requesting English" is code 1 (what the game maps to slot 0)
        code = {"english": 1, "french": 2, "spanish": 3, "german": 4, "italian": 5, "japanese": 0, "dutch": 6, "portuguese": 7}.get(want, 1)
        arms = {"A": ["--language", str(code)], "B": ["--language", str(CODE_OF_SLOT[want_bank[1]])],
                "C": ["--language", str(CODE_OF_SLOT[other_bank[1]])]}
    present = fx["present"]
    box = fx["region"]["box"]
    results = {}
    for tag, lang_args in arms.items():
        d, rc = run(a.bin, a.iso, out, fx, lang_args, tag)
        if rc != 0:
            print("language_judge: run %s exited %d" % (tag, rc), file=sys.stderr)
            return 2
        frame = os.path.join(d, "dump", "frame_%06d.ppm" % present)
        if not os.path.exists(frame):
            print("language_judge: run %s dumped no present %d" % (tag, present), file=sys.stderr)
            return 2
        h, lit = region_hash(frame, box)
        quads, text9, selected = parse_log(os.path.join(d, "run.log"), present)
        results[tag] = {"args": lang_args, "region_hash": h, "text_pixels": lit, "quad_hashes": sorted(quads), "text9": text9, "selected": selected}
        print(json.dumps({"run": tag, **results[tag]}))

    fails = []
    A, B, C = results["A"], results["B"], results["C"]
    if A["text_pixels"] < fx["min_text_pixels"]:
        fails.append("A's text region has %d text-coloured pixels < %d (no menu text drawn)" % (A["text_pixels"], fx["min_text_pixels"]))
    if A["region_hash"] != B["region_hash"]:
        fails.append("A (requested %s) draws other glyphs than B (the %s-content slot %d %s): region %s != %s -- the requested language is NOT shown"
                     % (want, want, want_bank[0], want_bank[1], A["region_hash"], B["region_hash"]))
    if A["region_hash"] == C["region_hash"]:
        fails.append("A (requested %s) draws the same glyphs as C (%s): region %s" % (want, other, A["region_hash"]))
    if A["quad_hashes"] or B["quad_hashes"] or C["quad_hashes"]:
        if A["quad_hashes"] != B["quad_hashes"]:
            fails.append("text-quad hash set of A != B")
        if A["quad_hashes"] == C["quad_hashes"]:
            fails.append("text-quad hash set of A == C")
    if A["text9"] is not None and A["text9"].lower() != expected_text9:
        fails.append("the game ended with text 9 '%s', the %s bank's is '%s'" % (A["text9"], want, expected_text9))
    if A["selected"] and A["selected"]["content"] != want:
        fails.append("the runner selected a %s bank for '%s'" % (A["selected"]["content"], want))
    if fx.get("text_row"):
        sel = A["selected"]
        if not sel or "speech" not in sel:
            fails.append("the runner reported no text / speech selection")
        else:
            if sel["slot"] != want_bank[0]:
                fails.append("A's text slot is %d, the %s text's is %d" % (sel["slot"], want, want_bank[0]))
            if sel["speech"] != fx["text_row"]["speech"] or sel["speech_how"] != "name":
                fails.append("A's speech is %s by %s, not %s by name: the text row did not leave the speech alone" % (sel["speech"], sel["speech_how"], fx["text_row"]["speech"]))
    verdict = "GREEN" if not fails else "RED"
    print(json.dumps({"verdict": verdict, "requested": want, "requested_slot": want_bank[0], "requested_slot_name": want_bank[1],
                      "other": other, "other_slot": other_bank[0], "present": present, "why": fails}))
    print("language_judge: %s (%s requested; A==B %s, A!=C %s; quad hashes %s)" % (
        verdict, want, A["region_hash"] == B["region_hash"], A["region_hash"] != C["region_hash"],
        "A %d / B %d / C %d" % (len(A["quad_hashes"]), len(B["quad_hashes"]), len(C["quad_hashes"]))))
    return 0 if not fails else 1


if __name__ == "__main__":
    sys.exit(main())
