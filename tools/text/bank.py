#!/usr/bin/env python3
"""bank.py -- the game's two text files (Language\\Code\\<slot>.txt, Language\\AgentLab\\<slot>.txt) for the translation tools.

english_texts(disc)         the ENGLISH texts as the game numbers them (read from the disc at run time; never written into the
                            repository): {"Code": [bytes...], "AgentLab": [...], "Port": the port's own row titles}, and the
                            slot they came from
mask(text) / unmask(text)   a text's control codes and placeholders as readable tokens for a translator, and back to the bytes
                            the game reads ({CROSS} <-> "\\", {SAVE} <-> "(xxx)", {PARA} <-> "~~", "~" kept as a line break)
key(text)                   sha1 of an English text: the TSV's second column (a translation is for exactly that English)
write_bank(...)             the language mod's bank (te_language_mod.h): Windows-1256, CR LF, one section per file
                            opened by "@@ <file> <texts> <sha256 of the English file's texts>"
"""
import hashlib
import re

from twin_disc import PAL_SLOTS, bank_lines, slot_of_language

FILES = ["Code", "AgentLab"]
# The port's own texts a language translates beside the game's: the Options menu's two language rows (te_language_mod.h
# PortEnglish()). Not the disc's: written here, the bank's third section ("Port", keyed by these English texts).
PORT = "Port"
PORT_TEXTS = [b"text language", b"speech language"]
BANK_FILES = FILES + [PORT]

# The game font's button glyphs and symbols: a translator writes the token, the bank gets the byte
TOKENS = [
    ("{SAVE}", b"(xxx)"),        # the save's name (SaveCode::MessageText)
    ("{KB}", b"(x)"),            # the save's size in kilobytes
    ("{UNKNOWN}", b"??????????"),  # a level not reached yet
    ("{PARA}", b"~~"),           # an empty line between two paragraphs
    ("{SQUARE}", b"["),
    ("{CROSS}", b"\\"),
    ("{CIRCLE}", b"]"),
    ("{TRIANGLE}", b"^"),
    ("{L1}", b"{"),
    ("{R1}", b"}"),
    ("{L2}", b"\xa6"),
    ("{R2}", b"\xac"),
    ("{LEFT}", b"<"),
    ("{RIGHT}", b">"),
    ("{REG}", b"@"),
]
TOKEN_RE = re.compile(r"\{(SAVE|KB|UNKNOWN|PARA|SQUARE|CROSS|CIRCLE|TRIANGLE|L1|R1|L2|R2|LEFT|RIGHT|REG)\}")


def english_texts(disc):
    """{file: [texts as bytes]} of the slot whose text IS English, and that slot's name."""
    slot, how = slot_of_language(disc, "english")
    if slot is None:
        raise ValueError("the disc has no English text bank (%s)" % how)
    out = {}
    for f in FILES:
        data = disc.archive_file("Language\\%s\\%s.txt" % (f, slot))
        if data is None:
            raise ValueError("no Language\\%s\\%s.txt" % (f, slot))
        out[f] = [l.encode("latin1") for l in bank_lines(data)]
    out[PORT] = list(PORT_TEXTS)
    return out, slot


def english_file_key(texts):
    """The sha256 the bank's section header names: the English file's texts joined by '\\n' (te_language_mod.h)."""
    return hashlib.sha256(b"\n".join(texts)).hexdigest()


def key(text):
    return hashlib.sha1(text).hexdigest()


def text_id(file, index):
    return "%s:%03d" % ({"Code": "code", "AgentLab": "agentlab", PORT: "port"}[file], index)


def mask(text):
    """English bytes -> the translator's text (str): tokens for the control codes, '~' kept (a line break the tools may move)."""
    s = text
    out = []
    i = 0
    while i < len(s):
        for tok, raw in TOKENS:
            if s.startswith(raw, i):
                out.append(tok)
                i += len(raw)
                break
        else:
            out.append(s[i:i + 1].decode("latin1"))
            i += 1
    return "".join(out)


def unmask(masked):
    """The translation (str, Arabic letters as Unicode) -> the bank's bytes (Windows-1256) or raise ValueError."""
    out = bytearray()
    pos = 0
    for m in TOKEN_RE.finditer(masked):
        out += masked[pos:m.start()].encode("cp1256")
        out += dict(TOKENS)["{%s}" % m.group(1)]
        pos = m.end()
    out += masked[pos:].encode("cp1256")
    return bytes(out)


def tokens_of(masked):
    """The placeholders a text carries (sorted), {PARA} included: a translation must carry the same."""
    return sorted(m.group(0) for m in TOKEN_RE.finditer(masked))


def bank_sections(english, tsv):
    """The bank's sections from a complete translation table: [(file, english texts, translated texts as bytes)] for the
    game's two files and the port's texts, in the bank's order."""
    return [(f, english[f], [unmask(tsv[text_id(f, i)][1]) for i in range(len(english[f]))]) for f in BANK_FILES]


def write_bank(path, sections):
    """sections: [(file, english texts, translated texts as bytes)] -> the bank file."""
    out = bytearray()
    for f, english, translated in sections:
        if len(english) != len(translated):
            raise ValueError("%s: %d translations for %d English texts" % (f, len(translated), len(english)))
        out += b"@@ %s %d %s\r\n" % (f.encode(), len(translated), english_file_key(english).encode())
        for t in translated:
            if not t or b"\r" in t or b"\n" in t:
                raise ValueError("%s: a text is empty or holds a line break" % f)
            out += t + b"\r\n"
    with open(path, "wb") as fh:
        fh.write(bytes(out))
    return len(out)


def read_tsv(path):
    """{id: (sha1, translation)} of a translation TSV (UTF-8: id, sha1 of the English, the translation; '#' comments)."""
    out = {}
    with open(path, encoding="utf-8") as fh:
        for n, line in enumerate(fh, 1):
            line = line.rstrip("\n")
            if not line or line.startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 3:
                raise ValueError("%s:%d: %d columns, a row is id, sha1, translation" % (path, n, len(parts)))
            out[parts[0]] = (parts[1], parts[2])
    return out


def write_tsv(path, rows, header):
    with open(path, "w", encoding="utf-8") as fh:
        for h in header:
            fh.write("# %s\n" % h)
        for rid, sha, text in rows:
            fh.write("%s\t%s\t%s\n" % (rid, sha, text))
