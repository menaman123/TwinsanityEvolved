#!/usr/bin/env python3
"""fit.py -- does a translation fit the box the game gives its text?

The game has no boxes: a text item is a place, a scale and an alignment, and its lines are as wide as their glyphs. The room a
text has is where the game DRAWS it: a menu row's column, a corner hint, a notice in the middle of the screen, the bottom
line of the levels. Texts drawn in the same place at the same scale form a CONTEXT, and the game's own layout of the widest
English text of a context is the room every text of that context has:

    box(text)   width = the widest English line of the text's context, in the glyph units the game sums (texels of the
                font at the item's scale: both languages are drawn at the same scale, so units compare directly)
                lines = the English text's own line count (at least 1): a translation never takes more lines
    fits        every line of the translation, shaped (shape.py) and measured with the glyphs it will be drawn with (the
                Arabic font's for Arabic forms and digits, the disc font's for the rest), within the width; no more lines

wrap(masked, room, lines) breaks a translation at SPACES only ('~'), paragraph breaks ({PARA}) kept, greedily.
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(HERE, "..", "font"))
import bank as B  # noqa: E402
import psf  # noqa: E402
import shape  # noqa: E402

GAME_FONT = "Startup\\Fonts\\Crash_Euro.psf"


def _ids(*ranges):
    out = []
    for r in ranges:
        out += list(range(r[0], r[1] + 1)) if isinstance(r, tuple) else [r]
    return out


# (context, file, text numbers, where the game draws them) -- from the decompilation's drawing code (menus.cpp DrawMenuPage,
# oleg.cpp the hint labels, savecode notices, commandsgame.cpp ShowTextLine)
CONTEXTS = [
    ("values", "Code", _ids((0, 8)), "a menu row's value, after the row's text and the separator"),
    ("rows", "Code", _ids((9, 27), (54, 55), (59, 65), (68, 70), 87, (89, 94), 96, (98, 177), (180, 182), (184, 187), 204),
     "menu rows and page titles: one line in the menu's column"),
    ("hints", "Code", _ids((28, 31), 178), "the hints in the bottom corners"),
    ("notices", "Code", _ids((32, 53), (56, 58), (66, 67), 95, 179, 183), "the controller / disc / memory card notices: centred lines"),
    ("levels", "Code", _ids((71, 86)), "level names"),
    ("slot_levels", "Code", _ids((188, 203)), "a save slot's level name: the game draws each word on a line of its own (oleg.cpp: every "
     "space becomes a line break); '_' is a blank glyph that keeps two words on one line"),
    ("symbols", "Code", _ids(88, 97), "symbols only (??????????, 100%): no words"),
    ("bottom", "AgentLab", list(range(0, 200)), "the levels' bottom text line: centred, three quarters of the menu size"),
]


def context_of(file, index):
    for name, f, ids, _ in CONTEXTS:
        if f == file and index in ids:
            return name
    # the port's own texts (bank.PORT) are the Options menu's language rows: menu rows
    return "rows" if file in ("Code", B.PORT) else "bottom"


# The screen caps a context's room: frame pixels per glyph texel where the game draws the context, MEASURED from the text
# census (TE_GFX_DIAG_TEXT=1 over fixtures/padlogs/hub_entry.padlog, English: a text's census box width over its texels --
# "three years ago..." 310 texels in 182.5 px, "continue without saving" 423 in 348.7, "select \\" 160 in 117.8), the 512-pixel
# frame, and the room the anchor leaves: a centred text the title-safe 90% of the frame, a corner hint from its corner
# (0.05 / 0.95) to the frame's middle. Some English bottom lines are wider than the frame at their scale (the game lets them
# run off the screen); a translation may not.
FRAME_PX = 512.0
SCREEN_CAPS = {
    "rows": (0.824, 0.90),
    "hints": (0.736, 0.45),
    "notices": (0.736, 0.90),
    "bottom": (0.589, 0.90),
}


def screen_cap(context):
    if context not in SCREEN_CAPS:
        return None
    px_per_texel, share = SCREEN_CAPS[context]
    return FRAME_PX * share / px_per_texel


class Metrics:
    """Glyph widths in texels: the disc font's by byte (its GlyphIndex rule), ours by codepoint (the font's table)."""

    def __init__(self, disc, arabic_font_path):
        game = psf.read_psf(disc.archive_file(GAME_FONT))
        self.first = game["first"]
        self.game = [g["w"] / 16.0 for g in game["glyphs"]]
        self.game_drawn = [g["page"] != 0 for g in game["glyphs"]]
        ours = psf.read_psf(open(arabic_font_path, "rb").read())
        self.arabic = {cp: g["w"] / 16.0 for cp, g in zip(ours["codepoints"], ours["glyphs"])}

    def game_width(self, byte):
        i = byte - self.first
        return self.game[i] if 0 <= i < len(self.game) else self.game[0]

    def glyph(self, cp, byte):
        """(width, font, missing) of a shaped glyph, as the text layer resolves it (te_text_shape.h Resolve)."""
        if cp in self.arabic:
            return self.arabic[cp], "arabic", False
        if 0x0600 <= cp <= 0x06FF or 0xFE70 <= cp <= 0xFEFF:
            return 0.0, "none", True
        i = byte - self.first
        if not 0 <= i < len(self.game):
            return 0.0, "none", True
        return self.game[i], "game", self.game[i] == 0 and byte != 0x20


# The contexts whose texts the game re-breaks before drawing: a save slot's level name, every space a line break
SPACES_BREAK = {"slot_levels"}


def as_drawn(context, text):
    """A text's bytes as the game draws them in its context."""
    return text.replace(b" ", b"~") if context in SPACES_BREAK else text


def english_lines(text):
    """The English text's lines (bytes) as RenderText makes them: split at '~', empty lines dropped."""
    return [l for l in text.split(b"~") if l]


def english_width(m, text):
    return max([sum(m.game_width(b) for b in line) for line in english_lines(text)] or [0.0])


def rooms(m, english):
    """{context: the room of its texts (texels)}: the widest English line of the context, capped by the screen."""
    out = {}
    for f in B.FILES:
        for i, t in enumerate(english[f]):
            c = context_of(f, i)
            out[c] = max(out.get(c, 0.0), english_width(m, as_drawn(c, t)))
    for c in out:
        cap = screen_cap(c)
        if cap is not None:
            out[c] = min(out[c], cap)
    return out


def measure(m, data):
    """[(line width, glyphs, missing)] of a translation's bytes, shaped as the game will draw them."""
    shaped = shape.shape_text(data)
    out = []
    for idx, line in enumerate(shaped["lines"]):
        if idx in shaped["empty"]:
            continue
        width, missing = 0.0, 0
        for cp, byte, _ in line:
            w, _, miss = m.glyph(cp, byte)
            width += w
            missing += 1 if miss else 0
        out.append((width, len(line), missing))
    return out


def fit(m, english, data, room):
    """(fits, reasons, widths, lines) of a translation against its English's box."""
    lines = measure(m, data)
    want_lines = max(1, len(english_lines(english)))
    reasons = []
    widest = max([w for w, _, _ in lines] or [0.0])
    if widest > room + 1e-6:
        reasons.append("a line is %.0f wide, the box %.0f" % (widest, room))
    if len(lines) > want_lines:
        reasons.append("%d lines, the English has %d" % (len(lines), want_lines))
    missing = sum(x for _, _, x in lines)
    if missing:
        reasons.append("%d glyph(s) neither font has" % missing)
    return not reasons, reasons, [round(w, 1) for w, _, _ in lines], len(lines)


def wrap(m, masked, room, max_lines):
    """The translation broken at SPACES to fit `room` (texels): its {PARA} breaks kept, each of its lines that fits kept as the
    translator broke it, a line too wide re-broken greedily: the masked text with '~' breaks."""
    out = []
    for p in masked.split("{PARA}"):
        lines = []
        for given in p.split("~"):
            words = [w for w in given.split(" ") if w]
            cur = []
            for w in words:
                trial = " ".join(cur + [w])
                if cur and max(x for x, _, _ in measure(m, B.unmask(trial))) > room:
                    lines.append(" ".join(cur))
                    cur = [w]
                else:
                    cur.append(w)
            if cur:
                lines.append(" ".join(cur))
        out.append("~".join(lines))
    return "{PARA}".join(out)
