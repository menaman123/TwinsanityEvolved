#!/usr/bin/env python3
"""language_tuple_judge.py -- do the TEXT language and the SPEECH language each drive their own part, chosen apart?

The retail game has one language index: the text files, the streamed speech bank (Crash6\\<slot>.mb), the FMV sound track (the
movie's channel = the index), a level's voices and the title cards all follow it. The PAL image used for development is a text-only patch
(Portuguese text in the `English` slot, the English text in the `Spanish` slot; every .mb/.pss retail), so a text language and a
speech language are two settings: the text slot by content, the voice slot (the retail index) by the speech bank's name. This
judge checks, per PAIR (fixture "pairs": a text language with ANOTHER speech language), on one deterministic headless run each
(`--text-language X --speech-language Y`, fixtures/padlogs/language_tuple.plan: title -> new game -> the intro movie opens ->
skipped -> the hub):

  1 TEXT   the title's text bank is X's: the runner's exit line `game text 9 = '<line>'` equals the disc's Code line 9 of the slot
           whose text IS X ("new game", "novo jogo", ...) -- read off the disc by the judge;
  2 FMV    the intro movie's selected sound track (`movie: \\FMV\\H01_A.PSS;1 opened: ... sound track N`, the Player's track id)
           is Y's: the slot NAMED for Y's speech;
  3 SPEECH the speech bank opened for the in-game lines (`[stream] open #k "Crash6\\<Name>.mb"`, TE_STREAM_TRACE: the stream
           census's bank) is Y's -- the bank every streamed speech line (SetVoiceBank -> StartMusic InVoiceBank) comes from;
  + LISTS  the run's language line lists the text languages and the speech languages the fixture freezes (by content; no
           "(... voice)" / "(... text)" labels: each row names one language);
  + APART  both were chosen BY NAME (neither fell back nor followed the other).

The expected slots come from the frozen fixture (fixtures/judges/language_tuple_pal.json, backed by
tools/judges/language_disc_evidence.py: .mb tracks byte-identical to the NTSC-U English bank, title cards identical, the FMV
track's correlation with the NTSC-U track) when the disc's slots match the fixture's, else derived from the disc (text slot by
content; voice slot = the slot named for the speech language with a Crash6\\<name>.mb).

RED before the tuple (2026-10-18, the voice followed the text slot); 2026-10-27 RED on the runner of 43e9989 (one
language for both, no --text-language), GREEN on its change.

Usage: language_tuple_judge.py --bin BIN --iso ISO [--pair TEXT+SPEECH ...] [--out DIR] [--fixture F]
Exit 0 = GREEN (every pair judged), 1 = RED, 2 = the run failed / the disc has no banks.
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "language_tuple_pal.json")

PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
NEW_GAME = {"english": "new game", "portuguese": "novo jogo", "french": "nouvelle partie", "german": "neues spiel",
            "italian": "nuova partita", "spanish": ("nuevo juego", "nueva partida", "juego nuevo"), "dutch": "nieuw spel"}

TEXT9 = re.compile(r"\[twinsanityevolved\] language: .*; game text 9 = '([^']*)'")
MOVIE = re.compile(r"\[twinsanityevolved\] movie: (\S+) opened: .* sound track (\d+) ")
BANK_OPEN = re.compile(r'\[stream\] open #(\d+) "Crash6\\(\w+)\.mb" -> found')
MUSIC_WINDOW = re.compile(r"\[audio\] music window file (\d+) ")
TEXTS = re.compile(r"; text languages((?: \[[^\]]*\])*)")
SPEECHES = re.compile(r"; speech languages((?: \[[^\]]*\])*)")
LIST_NAME = re.compile(r"\[(\w+): ")
SELECTED = re.compile(r"requested text '([^']*)' speech '([^']*)' -> text (\w+) by ([^:]+): text slot (\d+); speech (\w+) by ([^:]+): voice slot (\d+); Language\(\) (\d+)")
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


def disc_slots(iso):
    """{slot: {name, text, text9, speech, speech_bank}} for every PAL slot the disc carries a Code bank for."""
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
        slots = {}
        for slot, name in enumerate(PAL_SLOTS):
            key = "Language\\Code\\%s.txt" % name
            if key not in index:
                continue
            o, s = index[key]
            f.seek(base + o)
            lines = [l for l in re.split(r"[\r\n]+", f.read(min(s, 2048)).decode("latin1").lower()) if l]
            line9 = lines[9] if len(lines) > 9 else ""
            content = None
            for lang, words in NEW_GAME.items():
                if line9 in ((words,) if isinstance(words, str) else words):
                    content = lang
            mb = ("%s.MB" % name).upper()
            speech = mb in entries and entries[mb][2] > 0
            slots[slot] = {"name": name, "text": content or name.lower(), "text9": line9, "speech": name.lower() if speech else None,
                           "speech_bank": "Crash6\\%s.mb" % name if speech else None}
        return slots


def derive_lists(slots):
    """{text language: {text_slot}} and {speech language: {voice_slot, fmv_track, speech_bank}}, english first, then slot order."""
    texts, speeches = {}, {}
    for slot in sorted(slots):
        t = slots[slot]["text"]
        if t not in texts:
            texts[t] = {"text_slot": slot}
        sp = slots[slot]["speech"]
        if sp and sp not in speeches:
            speeches[sp] = {"voice_slot": slot, "fmv_track": slot, "speech_bank": slots[slot]["speech_bank"]}
    order = lambda d: {k: d[k] for k in (["english"] if "english" in d else []) + [k for k in d if k != "english"]}
    return order(texts), order(speeches)


def supports(binary, flag):
    try:
        r = subprocess.run([binary, "--help"], capture_output=True, text=True, timeout=60, stdin=subprocess.DEVNULL,
                           env=dict(os.environ, TE_GFX_HEADLESS="1"))
    except Exception:
        return False
    return flag in (r.stderr + r.stdout)


# ---- the run -------------------------------------------------------------------------------------------------------------
def run(binary, iso, out, fx, text, speech):
    tag = "%s+%s" % (text, speech)
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--trace-state", "--text-language", text,
            "--speech-language", speech, "--mc-dir", mc, "--autopilot", os.path.join(ROOT, fx["plan"]), "--stop-at", str(fx.get("stop_at", 12000)),
            "--run-seconds", str(fx.get("run_seconds", 900))]
    language = tag
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_STREAM_TRACE"] = "1"
    print("language_tuple_judge: run %s: %s" % (language, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("language_tuple_judge: run %s exit %d -> %s" % (language, rc, log), flush=True)
    return log, rc


def parse_log(path, intro):
    r = {"text9": None, "movies": [], "intro_track": None, "banks": [], "speech_bank": None, "speech_windows": 0, "texts": [],
         "speeches": [], "selected": None, "playing": False}
    bank_files = {}
    with open(path, encoding="latin-1") as f:
        for line in f:
            m = TEXT9.search(line)
            if m:
                r["text9"] = m.group(1)
            m = MOVIE.search(line)
            if m:
                r["movies"].append((m.group(1), int(m.group(2))))
                if m.group(1).upper() == intro.upper():
                    r["intro_track"] = int(m.group(2))
            m = BANK_OPEN.search(line)
            if m:
                name = m.group(2)
                if name.lower() != "music":
                    r["banks"].append(name)
                    bank_files[int(m.group(1))] = name
            m = MUSIC_WINDOW.search(line)
            if m and int(m.group(1)) in bank_files:
                r["speech_windows"] += 1
            if "[twinsanityevolved] language:" in line:
                mt, ms = TEXTS.search(line), SPEECHES.search(line)
                if mt and ms and not r["texts"]:
                    r["texts"], r["speeches"] = LIST_NAME.findall(mt.group(1)), LIST_NAME.findall(ms.group(1))
                m = SELECTED.search(line)
                if m:
                    r["selected"] = {"text": m.group(3), "how_text": m.group(4), "text_slot": int(m.group(5)), "speech": m.group(6),
                                     "how_speech": m.group(7), "voice_slot": int(m.group(8)), "code": int(m.group(9))}
            m = GSTATE.search(line)
            if m and m.group(2) == "Playing":
                r["playing"] = True
    # the speech bank the in-game lines stream from: the LAST voice bank opened before the hub (the title's SetVoiceBank; a
    # language change in the Options menu re-opens it)
    r["speech_bank"] = "Crash6\\%s.mb" % r["banks"][-1] if r["banks"] else None
    return r


def judge_one(text, speech, texts, speeches, slots, result, fx):
    fails = []
    t, sp = texts[text], speeches[speech]
    want9 = slots[t["text_slot"]]["text9"]
    if result["text9"] is None:
        fails.append("the runner printed no `game text 9` line (the run did not end at --stop-at)")
    elif result["text9"] != want9:
        fails.append("TEXT: the title's text 9 is '%s', not the %s text's '%s' (slot %d)" % (result["text9"], text, want9, t["text_slot"]))
    if result["intro_track"] is None:
        fails.append("FMV: the intro movie %s never opened (movies skipped? the plan did not reach NEW GAME?)" % fx["intro_movie"])
    elif result["intro_track"] != sp["fmv_track"]:
        fails.append("FMV: %s opened with sound track %d, not the %s speech's track %d" % (fx["intro_movie"], result["intro_track"], speech, sp["fmv_track"]))
    if result["speech_bank"] is None:
        fails.append("SPEECH: no Crash6\\<slot>.mb speech bank was opened (TE_STREAM_TRACE missing from the runner?)")
    elif result["speech_bank"].lower() != (sp["speech_bank"] or "").lower():
        fails.append("SPEECH: the speech bank opened is %s, not the %s speech's %s" % (result["speech_bank"], speech, sp["speech_bank"]))
    if not result["playing"]:
        fails.append("the run never reached Playing (the hub): the plan did not hold")
    if result["texts"] != list(texts) or result["speeches"] != list(speeches):
        fails.append("LISTS: text %s / speech %s, expected %s / %s" % (result["texts"], result["speeches"], list(texts), list(speeches)))
    sel = result["selected"]
    if not sel:
        fails.append("APART: the runner reported no text / speech selection")
    elif sel["text"] != text or sel["how_text"] != "name" or sel["speech"] != speech or sel["how_speech"] != "name":
        fails.append("APART: text %s by %s, speech %s by %s (expected %s and %s, both by name)" % (sel["text"], sel["how_text"], sel["speech"],
                                                                                              sel["how_speech"], text, speech))
    return fails


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--pair", action="append", help="TEXT+SPEECH (repeatable; default: the fixture's pairs)")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--out", help="the runs' directory (default: build/language_tuple_judge)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or os.path.join(ROOT, "build", "language_tuple_judge")
    os.makedirs(out, exist_ok=True)

    slots = disc_slots(a.iso)
    if not slots:
        print("language_tuple_judge: the disc carries no Language\\Code banks the judge can read", file=sys.stderr)
        return 2
    fx_slots = {int(k): v for k, v in fx["slots"].items()}
    same_disc = all(k in slots and slots[k]["text"] == v["text"] and slots[k]["speech"] == v["speech"] for k, v in fx_slots.items()) and len(slots) == len(fx_slots)
    if same_disc:
        texts, speeches = fx["texts"], fx["speeches"]
        print("language_tuple_judge: the disc's slots match the frozen fixture (%s): its evidence-backed slots stand" % os.path.relpath(a.fixture, ROOT))
    else:
        texts, speeches = derive_lists(slots)
        print("language_tuple_judge: another disc than the fixture's: the lists are derived from its slots")
    for slot in sorted(slots):
        print(json.dumps({"disc_slot": slot, **slots[slot]}))
    print(json.dumps({"texts": texts, "speeches": speeches}))
    pairs = [p.split("+", 1) for p in a.pair] if a.pair else fx["pairs"]
    for t, sp in pairs:
        if t not in texts or sp not in speeches:
            print("language_tuple_judge: the disc has no text %s / speech %s (it has %s / %s)" % (t, sp, list(texts), list(speeches)), file=sys.stderr)
            return 2
    if not supports(a.bin, "--text-language"):
        print(json.dumps({"verdict": "red", "pairs": pairs, "bin": a.bin, "reasons": ["the runner has no --text-language / --speech-language: "
                                                                                     "one language drives the text and the speech"]}))
        print("language_tuple_judge: RED (the text and the speech cannot be chosen apart)")
        return 1

    verdicts = {}
    all_fails = []
    for t, sp in pairs:
        tag = "%s+%s" % (t, sp)
        log, rc = run(a.bin, a.iso, out, fx, t, sp)
        if rc != 0:
            print("language_tuple_judge: run %s exited %d" % (tag, rc), file=sys.stderr)
            return 2
        result = parse_log(log, fx["intro_movie"])
        fails = judge_one(t, sp, texts, speeches, slots, result, fx)
        verdicts[tag] = {"text9": result["text9"], "intro_track": result["intro_track"], "speech_bank": result["speech_bank"],
                         "speech_windows": result["speech_windows"], "banks_opened": result["banks"], "texts": result["texts"],
                         "speeches": result["speeches"], "selected": result["selected"], "verdict": "green" if not fails else "red", "reasons": fails}
        print(json.dumps({"run": tag, **verdicts[tag]}))
        all_fails += ["%s: %s" % (tag, f) for f in fails]

    verdict = "GREEN" if not all_fails else "RED"
    print(json.dumps({"verdict": verdict.lower(), "pairs": pairs, "bin": a.bin, "reasons": all_fails}))
    print("language_tuple_judge: %s (%s)" % (verdict, ", ".join("%s %s" % (k, v["verdict"]) for k, v in verdicts.items())))
    return 0 if not all_fails else 1


if __name__ == "__main__":
    sys.exit(main())
