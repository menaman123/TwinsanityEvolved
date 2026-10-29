#!/usr/bin/env python3
"""twin_disc.py -- read your disc image at run time (READ ONLY): the ISO9660 directory and the game's archive.

The game keeps every level chunk, text bank, font and texture inside CRASH6/CRASH.BD, indexed by CRASH6/CRASH.BH:
    u32 header (not a count the reader needs); then per entry: u32 name length, the name ("Language\\Code\\English.txt"),
    u32 offset into CRASH.BD, u32 size.
(the same walk platform/nativeee/host/language.cpp and the language judges make). Nothing read here is ever written into
the repository: the tools that use this module measure (font style), verify keys (sha1 of a string) or build OUR files.

    from twin_disc import Disc
    disc = Disc(iso_path)
    data = disc.archive_file("Language\\Code\\Spanish.txt")
"""
import struct

SECTOR = 2048


class Disc:
    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        self.f.seek(16 * SECTOR)
        pvd = self.f.read(SECTOR)
        if pvd[1:6] != b"CD001":
            raise ValueError("%s: not an ISO9660 image (no primary volume descriptor)" % path)
        self.root = (struct.unpack_from("<I", pvd, 156 + 2)[0], struct.unpack_from("<I", pvd, 156 + 10)[0])
        self._index = None

    def close(self):
        self.f.close()

    # ---- ISO9660 --------------------------------------------------------------------------------------------------
    def listdir(self, lba, size):
        """[(name, lba, size, is_dir)] of a directory extent (names without ';1')."""
        self.f.seek(lba * SECTOR)
        d = self.f.read(size)
        out, off = [], 0
        while off < len(d):
            ln = d[off]
            if ln == 0:
                off = (off // SECTOR + 1) * SECTOR
                continue
            e_lba = struct.unpack_from("<I", d, off + 2)[0]
            e_size = struct.unpack_from("<I", d, off + 10)[0]
            nl = d[off + 32]
            raw = d[off + 33:off + 33 + nl]
            if raw not in (b"\x00", b"\x01"):
                out.append((raw.decode("latin1").split(";")[0], e_lba, e_size, bool(d[off + 25] & 2)))
            off += ln
        return out

    def find(self, path):
        """(lba, size, is_dir) of a disc path ("CRASH6/CRASH.BD", any case), or None."""
        lba, size, is_dir = self.root[0], self.root[1], True
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            if not is_dir:
                return None
            hit = [e for e in self.listdir(lba, size) if e[0].upper() == part.upper()]
            if not hit:
                return None
            _, lba, size, is_dir = hit[0]
        return lba, size, is_dir

    def read(self, offset, count):
        self.f.seek(offset)
        return self.f.read(count)

    def disc_file(self, path):
        e = self.find(path)
        if e is None or e[2]:
            return None
        return self.read(e[0] * SECTOR, e[1])

    # ---- the game's archive (CRASH6/CRASH.BH -> CRASH6/CRASH.BD) ---------------------------------------------------
    def index(self):
        """{name: (offset, size)} of the archive, names as the BH writes them (case kept)."""
        if self._index is not None:
            return self._index
        bh = self.disc_file("CRASH6/CRASH.BH")
        bd = self.find("CRASH6/CRASH.BD")
        if bh is None or bd is None:
            raise ValueError("%s: no CRASH6/CRASH.BH + CRASH.BD (not a Crash Twinsanity disc?)" % self.path)
        self._bd_base = bd[0] * SECTOR
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
        self._index = index
        return index

    def archive_name(self, name):
        """The archive's spelling of a name looked up case-insensitively (either slash), or None."""
        want = name.replace("/", "\\").lower()
        for n in self.index():
            if n.lower() == want:
                return n
        return None

    def archive_file(self, name):
        n = self.archive_name(name)
        if n is None:
            return None
        o, s = self.index()[n]
        return self.read(self._bd_base + o, s)


# The PAL executable's language slots (GlobalLanguagesArray): Language\Code\<slot>.txt
PAL_SLOTS = ["English", "French", "German", "Spanish", "Italian"]
# Line 9 of a Code bank (the main menu's "new game") per language: how a slot's CONTENT is recognised (the PAL image used for development is
# a translation patch: its `English` slot holds Portuguese and the English text is in the `Spanish` slot)
NEW_GAME = {"english": "new game", "portuguese": "novo jogo", "french": "nouvelle partie", "german": "neues spiel",
            "italian": "nuova partita", "spanish": "nuevo juego"}


def bank_lines(data):
    """The game's ReadTextFile rule: the text split at its line breaks, blank lines are not lines (a text's number counts
    the lines that are not blank)."""
    text = data.decode("latin1")
    return [l for l in text.replace("\r\n", "\n").replace("\r", "\n").split("\n") if l != ""]


def slot_of_language(disc, language):
    """The slot whose Code bank IS `language` (by its line 9), else the slot NAMED for it; (slot name, how) or (None, why)."""
    want = language.lower()
    by_name = None
    for slot in PAL_SLOTS:
        data = disc.archive_file("Language\\Code\\%s.txt" % slot)
        if data is None:
            continue
        lines = bank_lines(data)
        if len(lines) > 9 and lines[9].lower() == NEW_GAME.get(want, "\0"):
            return slot, "content (line 9 '%s')" % lines[9]
        if slot.lower() == want:
            by_name = slot
    if by_name:
        return by_name, "slot name"
    return None, "no %s bank on this disc" % want
