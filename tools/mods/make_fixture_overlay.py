#!/usr/bin/env python3
"""make_fixture_overlay.py -- the disc overlay's test mods, made from your own disc image at TEST time.

Writes complete mod folders (mod.json schema 2, kind disc, source kind "folder", a CRASH6/CRASH.BD.ranges
recipe and the few bytes it names under data/) into --out. They are NEVER committed: the judge
(tools/judges/mods_overlay_judge.py) makes them in a temporary folder from $ISO and deletes them. Nothing of the disc is
copied into them: each recipe names the disc's own bytes as `base` ranges and carries only the bytes it changes.

  textbank     one English menu string changed in the text bank the OPTIONS page shows: the line "graphic options" of the
               Language\\Code\\<slot>.txt bank(s) holding it (found through CRASH6/CRASH.BH as the game's language setting
               finds them) -> "graphic optionz"; same length, so the archive index (CRASH.BH) is untouched.
  levelchunk   one byte of a level chunk the hub route loads (Levels\\Earth\\Hub\\beach.rm2): the VERSION of its root
               section header (bits 16-31 of the chunk's first word: union SectionFormat { type : 16; version : 16 },
               game/include/game/readers.h) flipped between 0 and 1. Harmless by construction: the game's only use of a
               section's version is `format.version >= SectionVersions` (2) in ReadSectionHeader (game/src/game/readers.cpp)
               -- versions 0 and 1 read the same -- so the chunk loads exactly as before while its bytes come from the overlay.
  order_a/_b   two layers touching the same string: A makes it "graphic optiona", B "graphic optionb" and B also changes
               "sound options" -> "sound optionz" (so what B changes elsewhere survives under A): mods.order decides which
               string wins, and both orders compose (every recipe keeps the bank's length).

Usage: make_fixture_overlay.py --iso ISO --out DIR [--kinds textbank,levelchunk,order] [--string "graphic options"]
       [--chunk 'Levels\\Earth\\Hub\\beach.rm2']
Writes DIR/<id>/... per mod and DIR/report.json (what was changed where). Exit 0, or 2 with the reason.
"""
import argparse
import json
import os
import struct
import sys

SECTOR = 2048


class Iso:
    """A read-only ISO 9660 image (2048-byte sectors, or a raw 2352-byte image)."""

    def __init__(self, path):
        self.f = open(path, "rb")
        head = self.f.read(16)
        self.raw = head[:12] == b"\x00" + b"\xff" * 10 + b"\x00"
        self.stride = 2352 if self.raw else SECTOR
        self.user = (24 if head[15] == 2 else 16) if self.raw else 0
        pvd = self.sector(16)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("%s: no ISO 9660 primary volume descriptor" % path)
        self.root = self._record(pvd, 156)

    def sector(self, lba, count=1):
        out = bytearray()
        for k in range(count):
            self.f.seek((lba + k) * self.stride + self.user)
            out += self.f.read(SECTOR)
        return bytes(out)

    def read(self, lba, offset, size):
        """`size` bytes of the file at `lba`, from `offset` in it."""
        if not self.raw:
            self.f.seek(lba * SECTOR + offset)
            return self.f.read(size)
        out = bytearray()
        pos = offset
        while len(out) < size:
            s, inside = divmod(pos, SECTOR)
            take = min(SECTOR - inside, size - len(out))
            out += self.sector(lba + s)[inside:inside + take]
            pos += take
        return bytes(out)

    @staticmethod
    def _record(data, at):
        length = data[at]
        lba = struct.unpack_from("<I", data, at + 2)[0]
        size = struct.unpack_from("<I", data, at + 10)[0]
        flags = data[at + 25]
        name = data[at + 33:at + 33 + data[at + 32]].decode("latin-1")
        return {"length": length, "lba": lba, "size": size, "dir": bool(flags & 2), "name": name}

    def listdir(self, entry):
        data = self.sector(entry["lba"], (entry["size"] + SECTOR - 1) // SECTOR)
        out, at = [], 0
        while at < entry["size"]:
            if data[at] == 0:
                at = (at // SECTOR + 1) * SECTOR
                continue
            rec = self._record(data, at)
            if rec["name"] not in ("\x00", "\x01"):
                out.append(rec)
            at += rec["length"]
        return out

    def find(self, path):
        cur = self.root
        for part in [p for p in path.replace("\\", "/").split("/") if p]:
            hit = [e for e in self.listdir(cur) if e["name"].split(";")[0].upper() == part.upper()]
            if not hit:
                return None
            cur = hit[0]
        return cur


def archive(iso):
    """CRASH6/CRASH.BH's index of CRASH6/CRASH.BD: [(name, offset, size)]."""
    bh, bd = iso.find("CRASH6/CRASH.BH"), iso.find("CRASH6/CRASH.BD")
    if not bh or not bd:
        raise ValueError("CRASH6/CRASH.BH or CRASH6/CRASH.BD is not on the disc")
    index = iso.read(bh["lba"], 0, bh["size"])
    rows, at = [], 4   # a 32-bit word the game's reader skips, then {length, name, offset, size}*
    while at + 4 <= len(index):
        n = struct.unpack_from("<i", index, at)[0]
        at += 4
        if n <= 0 or at + n + 8 > len(index):
            break
        name = index[at:at + n].decode("latin-1")
        at += n
        offset, size = struct.unpack_from("<II", index, at)
        at += 8
        rows.append((name, offset, size))
    return bd, rows


def line_offsets(text, wanted):
    """Byte offsets of every line of `text` equal to `wanted` (case-insensitive; CR LF or LF lines)."""
    out, at = [], 0
    for raw in text.split(b"\n"):
        line = raw[:-1] if raw.endswith(b"\r") else raw
        if line.decode("latin-1").lower() == wanted.lower():
            out.append(at)
        at += len(raw) + 1
    return out


def write_mod(out, mod_id, name, description, bd_size, segments, payloads):
    """A mod folder: mod.json, disc/CRASH6/CRASH.BD.ranges (the segments), data/<file> (the payloads)."""
    root = os.path.join(out, mod_id)
    os.makedirs(os.path.join(root, "disc", "CRASH6"), exist_ok=True)
    os.makedirs(os.path.join(root, "data"), exist_ok=True)
    for rel, data in payloads.items():
        with open(os.path.join(root, rel), "wb") as f:
            f.write(data)
    total = 0
    lines = ["# %s: CRASH6/CRASH.BD as the disc's bytes with %d changed range(s); the composed size is the disc's" % (mod_id, len(payloads))]
    for seg in segments:
        if seg[0] == "base":
            lines.append("base %d %d" % (seg[1], seg[2]))
            total += seg[2]
        else:
            lines.append("file %s %d %d" % (seg[1], seg[2], seg[3]))
            total += seg[3]
    if total != bd_size:
        raise ValueError("%s: the recipe composes %d bytes, the archive has %d" % (mod_id, total, bd_size))
    with open(os.path.join(root, "disc", "CRASH6", "CRASH.BD.ranges"), "w", newline="\n") as f:
        f.write("\n".join(lines) + "\n")
    manifest = {
        "schema": 2, "id": mod_id, "kind": "disc", "name": name, "author": "the disc overlay judge", "version": "1",
        "description": description,
        "source": {"kind": "folder", "artefact": mod_id, "sha256": ""},
        "base": {"region": "PAL", "executable": "SLES_525.68"},
        "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges", "bytes": bd_size}],
    }
    with open(os.path.join(root, "mod.json"), "w", newline="\n") as f:
        json.dump(manifest, f, indent=2)
        f.write("\n")
    return root


def recipe(bd_size, changes):
    """The segments of a recipe: the disc's bytes around each (absolute offset, payload name, bytes) change, in order."""
    segments, cursor, payloads = [], 0, {}
    for at, rel, data in sorted(changes):
        if at < cursor:
            raise ValueError("overlapping changes")
        if at > cursor:
            segments.append(("base", cursor, at - cursor))
        segments.append(("file", rel, 0, len(data)))
        payloads[rel] = data
        cursor = at + len(data)
    if cursor < bd_size:
        segments.append(("base", cursor, bd_size - cursor))
    return segments, payloads


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--kinds", default="textbank,levelchunk,order")
    ap.add_argument("--string", default="graphic options", help="the menu line changed (its last letter is replaced)")
    ap.add_argument("--second", default="sound options", help="the line order_b also changes")
    ap.add_argument("--chunk", default="Levels\\Earth\\Hub\\beach.rm2", help="the level chunk whose root section version flips")
    a = ap.parse_args()
    kinds = [k for k in a.kinds.split(",") if k]
    try:
        iso = Iso(a.iso)
        if not iso.find("SLES_525.68"):
            raise ValueError("SLES_525.68 is not on the disc: these fixtures are for the PAL disc (SLES-52568)")
        bd, rows = archive(iso)
    except (OSError, ValueError) as e:
        print("make_fixture_overlay: %s" % e, file=sys.stderr)
        return 2
    bd_size = bd["size"]
    report = {"iso": os.path.abspath(a.iso), "archive_bytes": bd_size, "entries": len(rows), "mods": {}}
    os.makedirs(a.out, exist_ok=True)

    def banks_with(line):
        hits = []
        for name, offset, size in rows:
            if name.lower().startswith("language\\code\\") and name.lower().endswith(".txt"):
                text = iso.read(bd["lba"], offset, size)
                for at in line_offsets(text, line):
                    hits.append((name, offset, at))
        return hits

    try:
        if "textbank" in kinds or "order" in kinds:
            first = banks_with(a.string)
            if not first:
                raise ValueError("no Language\\Code bank has the line '%s'" % a.string)
            new = a.string[:-1] + "z"
            if "textbank" in kinds:
                changes = [(off + at, "data/text%d.bin" % k, new.encode("latin-1")) for k, (name, off, at) in enumerate(first)]
                segments, payloads = recipe(bd_size, changes)
                root = write_mod(a.out, "textbank", "text bank test",
                                 "the menu line '%s' reads '%s' in %s (same length: the index is untouched)" % (a.string, new, ", ".join(n for n, _, _ in first)),
                                 bd_size, segments, payloads)
                report["mods"]["textbank"] = {"dir": root, "from": a.string, "to": new,
                                              "banks": [{"bank": n, "entry_offset": o, "line_offset": at} for n, o, at in first]}
            if "order" in kinds:
                second = banks_with(a.second)
                if not second:
                    raise ValueError("no Language\\Code bank has the line '%s'" % a.second)
                a_new, b_new, s_new = a.string[:-1] + "a", a.string[:-1] + "b", a.second[:-1] + "z"
                ca = [(off + at, "data/a%d.bin" % k, a_new.encode("latin-1")) for k, (n, off, at) in enumerate(first)]
                cb = [(off + at, "data/b%d.bin" % k, b_new.encode("latin-1")) for k, (n, off, at) in enumerate(first)]
                cb += [(off + at, "data/s%d.bin" % k, s_new.encode("latin-1")) for k, (n, off, at) in enumerate(second)]
                for mod_id, changes, what in (("order_a", ca, "'%s' -> '%s'" % (a.string, a_new)),
                                               ("order_b", cb, "'%s' -> '%s', '%s' -> '%s'" % (a.string, b_new, a.second, s_new))):
                    segments, payloads = recipe(bd_size, changes)
                    root = write_mod(a.out, mod_id, mod_id.replace("_", " "), "the order test: " + what, bd_size, segments, payloads)
                    report["mods"][mod_id] = {"dir": root, "changes": what}
                report["mods"]["order"] = {"a_wins": a_new, "b_wins": b_new, "b_also": s_new}
        if "levelchunk" in kinds:
            hit = [r for r in rows if r[0].lower() == a.chunk.lower()]
            if not hit:
                raise ValueError("no archive entry '%s'" % a.chunk)
            name, offset, size = hit[0]
            word = struct.unpack("<I", iso.read(bd["lba"], offset, 4))[0]
            kind, version = word & 0xFFFF, word >> 16
            if version not in (0, 1):
                raise ValueError("%s: its root section version is %d, not 0 or 1 (the flip would not be harmless)" % (name, version))
            flipped = version ^ 1
            segments, payloads = recipe(bd_size, [(offset + 2, "data/version.bin", bytes([flipped]))])
            root = write_mod(a.out, "levelchunk", "level chunk test",
                             "%s's root section version %d -> %d (read only as `version >= 2`: harmless)" % (name, version, flipped),
                             bd_size, segments, payloads)
            report["mods"]["levelchunk"] = {"dir": root, "chunk": name, "entry_offset": offset, "entry_size": size,
                                            "section_type": kind, "version_from": version, "version_to": flipped,
                                            "archive_offset": offset + 2}
    except (OSError, ValueError) as e:
        print("make_fixture_overlay: %s" % e, file=sys.stderr)
        return 2
    with open(os.path.join(a.out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    for mod_id, info in report["mods"].items():
        print("make_fixture_overlay: %s %s" % (mod_id, {k: v for k, v in info.items() if k != "dir"}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
