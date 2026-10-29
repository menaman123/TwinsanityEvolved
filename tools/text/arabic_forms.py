#!/usr/bin/env python3
"""arabic_forms.py -- the Arabic letters the language mod draws, their joining classes and contextual forms.

ONE table for the font pipeline (which glyphs exist), the translation tools (the width of a shaped string) and the documents.
The game's text layer has the same rules in C++ (platform/nativeee/wasm/text_shape.cpp); the unit test te_text_shape_test and
tools/text/translate.py --check (through the shaped-forms fixture) keep the two in agreement.

Joining classes (Unicode ArabicShaping): D dual-joining (four forms), R right-joining (two forms: it never joins the letter
after it), U non-joining (hamza), C join-causing (tatweel). ALEF MAKSURA is R here: in Arabic it only ends a word.
Forms are named by their PRESENTATION FORM codepoint (Unicode's Arabic Presentation Forms-B, U+FE80..U+FEFC): isolated,
final, initial, medial in that order for a D letter; isolated, final for an R letter; the lam-alef ligatures isolated + final.
"""

# letter: (joining class, isolated presentation form). Forms of a D letter: iso, fin, ini, med = base, base+1, base+2, base+3;
# of an R letter: iso, fin = base, base+1.
LETTERS = {
    0x0621: ("U", 0xFE80),  # hamza
    0x0622: ("R", 0xFE81),  # alef with madda above
    0x0623: ("R", 0xFE83),  # alef with hamza above
    0x0624: ("R", 0xFE85),  # waw with hamza above
    0x0625: ("R", 0xFE87),  # alef with hamza below
    0x0626: ("D", 0xFE89),  # yeh with hamza above
    0x0627: ("R", 0xFE8D),  # alef
    0x0628: ("D", 0xFE8F),  # beh
    0x0629: ("R", 0xFE93),  # teh marbuta
    0x062A: ("D", 0xFE95),  # teh
    0x062B: ("D", 0xFE99),  # theh
    0x062C: ("D", 0xFE9D),  # jeem
    0x062D: ("D", 0xFEA1),  # hah
    0x062E: ("D", 0xFEA5),  # khah
    0x062F: ("R", 0xFEA9),  # dal
    0x0630: ("R", 0xFEAB),  # thal
    0x0631: ("R", 0xFEAD),  # reh
    0x0632: ("R", 0xFEAF),  # zain
    0x0633: ("D", 0xFEB1),  # seen
    0x0634: ("D", 0xFEB5),  # sheen
    0x0635: ("D", 0xFEB9),  # sad
    0x0636: ("D", 0xFEBD),  # dad
    0x0637: ("D", 0xFEC1),  # tah
    0x0638: ("D", 0xFEC5),  # zah
    0x0639: ("D", 0xFEC9),  # ain
    0x063A: ("D", 0xFECD),  # ghain
    0x0641: ("D", 0xFED1),  # feh
    0x0642: ("D", 0xFED5),  # qaf
    0x0643: ("D", 0xFED9),  # kaf
    0x0644: ("D", 0xFEDD),  # lam
    0x0645: ("D", 0xFEE1),  # meem
    0x0646: ("D", 0xFEE5),  # noon
    0x0647: ("D", 0xFEE9),  # heh
    0x0648: ("R", 0xFEED),  # waw
    0x0649: ("R", 0xFEEF),  # alef maksura
    0x064A: ("D", 0xFEF1),  # yeh
}
TATWEEL = 0x0640
LAM = 0x0644
# alef variant -> (lam-alef isolated, lam-alef final)
LAM_ALEF = {0x0622: (0xFEF5, 0xFEF6), 0x0623: (0xFEF7, 0xFEF8), 0x0625: (0xFEF9, 0xFEFA), 0x0627: (0xFEFB, 0xFEFC)}
DIGITS = list(range(0x0660, 0x066A))        # Arabic-Indic digits: ASCII digits in a right-to-left string draw as these
PUNCTUATION = [0x060C, 0x061B, 0x061F]     # Arabic comma, semicolon, question mark (their own shapes)
MIRRORED = {ord("("): ord(")"), ord(")"): ord("(")}   # the only mirrored pair: [ ] { } < > are the game's button icons

ISO, FIN, INI, MED = 0, 1, 2, 3
FORM_NAMES = ["isol", "fina", "init", "medi"]


def forms_of(letter):
    """[(form index, presentation codepoint)] a letter has."""
    cls, base = LETTERS[letter]
    if cls == "D":
        return [(ISO, base), (FIN, base + 1), (INI, base + 2), (MED, base + 3)]
    if cls == "R":
        return [(ISO, base), (FIN, base + 1)]
    return [(ISO, base)]


def required_codepoints():
    """Every codepoint the Arabic font must carry, in its glyph order: the letters' forms, the lam-alef ligatures, tatweel,
    the Arabic-Indic digits, the Arabic punctuation and the word space (140)."""
    out = []
    for letter in sorted(LETTERS):
        out += [cp for _, cp in forms_of(letter)]
    for alef in sorted(LAM_ALEF):
        out += list(LAM_ALEF[alef])
    out.append(TATWEEL)
    out += DIGITS
    out += PUNCTUATION
    out.append(0x20)   # the word space of Arabic text: the typeface's own width (drawn as nothing)
    return out


def presentation_info():
    """{presentation codepoint: (base letter, form index)} (lam-alef: (alef, ISO|FIN) with the letter LAM implied)."""
    info = {}
    for letter in LETTERS:
        for form, cp in forms_of(letter):
            info[cp] = (letter, form)
    for alef, (iso, fin) in LAM_ALEF.items():
        info[iso] = (alef, ISO)
        info[fin] = (alef, FIN)
    return info


def is_arabic_letter(cp):
    return cp in LETTERS or cp == TATWEEL


def joins_left(cp):
    """Does the letter connect to the letter AFTER it (on its left)?"""
    if cp == TATWEEL:
        return True
    return cp in LETTERS and LETTERS[cp][0] == "D"


def joins_right(cp):
    """Does the letter connect to the letter BEFORE it (on its right)?"""
    if cp == TATWEEL:
        return True
    return cp in LETTERS and LETTERS[cp][0] in ("D", "R")


def shape_word_forms(cps):
    """Contextual forms of a logical sequence (letters only matter; anything else breaks the joining): a list of
    presentation codepoints (non-Arabic codepoints pass through), lam + alef as one ligature."""
    out = []
    i, n = 0, len(cps)
    while i < n:
        c = cps[i]
        if c not in LETTERS and c != TATWEEL:
            out.append(c)
            i += 1
            continue
        prev = cps[i - 1] if i > 0 else None
        join_prev = prev is not None and joins_left(prev) and joins_right(c)
        if c == LAM and i + 1 < n and cps[i + 1] in LAM_ALEF:
            iso, fin = LAM_ALEF[cps[i + 1]]
            out.append(fin if join_prev else iso)
            i += 2
            continue
        if c == TATWEEL:
            out.append(TATWEEL)
            i += 1
            continue
        nxt = cps[i + 1] if i + 1 < n else None
        join_next = nxt is not None and joins_left(c) and joins_right(nxt)
        cls, base = LETTERS[c]
        if cls == "D":
            form = MED if (join_prev and join_next) else INI if join_next else FIN if join_prev else ISO
        elif cls == "R":
            form = FIN if join_prev else ISO
        else:
            form = ISO
        out.append(base + form)
        i += 1
    return out
