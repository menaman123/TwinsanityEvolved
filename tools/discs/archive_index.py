#!/usr/bin/env python3
"""tools/discs/archive_index.py -- a disc's ARCHIVE INDEX as a table: every entry of CRASH6/CRASH.BH (its name and size, in the
index's order), the program SYSTEM.CNF boots, the whole image's identity and where the table came from, as JSON
(launcher/discs/archives/<serial>.json). Metadata only -- names and sizes, never a byte of the entries -- so the table may
live in the repository (tools/ci/asset_scan.py passes over it).

The mod importer (engine/app/host/mod_import) builds these tables in (mod_import.cmake embeds launcher/discs/archives/*.json)
to tell which release a LOOSE archive entry was made from: a file named like an entry of the
player's disc whose size is nearer the other release's entry of that name than this disc's, and less than half of which is
this disc's entry's bytes, is refused ("The file Default.rm2 matches the European disc's entry ..., not this disc's").

A table is labelled by PROVENANCE, like the file lists: "retail": true only when the whole image matches a known
retail dump of launcher/discs/redump.json (size, CRC-32 and SHA-1); otherwise "retail": false and --provenance says what the
image is (the PAL image used for development is a modified one: its text banks may differ from the retail disc's).

  tools/discs/archive_index.py IMAGE [--provenance TEXT] [--out launcher/discs/archives/<serial>.json]
  tools/discs/archive_index.py IMAGE --check launcher/discs/archives/<serial>.json   # exit 0 = the disc's index is the table

Read only; a plain .iso or a raw .bin (manifest.py's reader).
"""
import argparse
import json
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import manifest  # noqa: E402  (the disc reader, the program, the Redump check)

INDEX, DATA = "CRASH6/CRASH.BH", "CRASH6/CRASH.BD"


def read_index(img, files):
    by_path = {p: (lba, n) for p, lba, n in files}
    if INDEX not in by_path or DATA not in by_path:
        raise SystemExit("archive_index: the disc has no %s and %s" % (INDEX, DATA))
    lba, n = by_path[INDEX]
    bh = img.read(lba * manifest.SECTOR, n)
    header = struct.unpack_from("<i", bh, 0)[0]
    entries, at = [], 4
    while at < len(bh):
        k = struct.unpack_from("<i", bh, at)[0]
        if k <= 0 or at + 12 + k > len(bh):
            raise SystemExit("archive_index: %s does not read as this game's index (an entry at byte %d)" % (INDEX, at))
        name = bh[at + 4:at + 4 + k].decode("latin-1")
        offset, size = struct.unpack_from("<II", bh, at + 4 + k)
        entries.append((name, offset, size))
        at += 12 + k
    end = max((o + s for _, o, s in entries), default=0)
    if end > by_path[DATA][1]:
        raise SystemExit("archive_index: %s names bytes past the end of %s" % (INDEX, DATA))
    return header, entries, by_path[DATA][1]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--provenance", default="", help="what the image is, when it is not a known retail dump")
    ap.add_argument("--redump", default=manifest.DEFAULT_REDUMP)
    ap.add_argument("--out", default="")
    ap.add_argument("--check", default="", help="compare the disc's index with this table (names and sizes)")
    a = ap.parse_args()
    img = manifest.Image(a.image)
    files, cnf, exe, _ = manifest.describe(img, a.image)
    serial = manifest.serial_of(exe)
    if not serial:
        raise SystemExit("archive_index: SYSTEM.CNF names no program of a known serial (%r)" % exe)
    header, entries, data_bytes = read_index(img, files)
    if a.check:
        with open(a.check) as f:
            table = json.load(f)
        want = [tuple(e) for e in table.get("entries", [])]
        got = [(n, s) for n, _, s in entries]
        if want == got and table.get("serial") == serial:
            print("archive_index: %s: the disc's %d entries are the table's" % (a.check, len(got)))
            return 0
        print("archive_index: %s: the disc's index differs (%d entries vs %d)" % (a.check, len(got), len(want)))
        return 1
    image = img.image_hashes()
    match, _, words = manifest.against_redump(a.redump, serial, image)
    if not match and not a.provenance:
        raise SystemExit("archive_index: the image is not a known retail dump (%s): say what it is with --provenance" % words)
    region = manifest.REGIONS.get(serial[:4], "any")
    table = {
        "kind": "archive-index",
        "format": 1,
        "note": "the archive index of one release (CRASH6/CRASH.BH): every entry's name and size in the index's order -- "
                "metadata, no byte of the game; made by tools/discs/archive_index.py, built into the mod importer",
        "serial": serial,
        "executable": exe,
        "region": region,
        "version": cnf.get("VER", ""),
        "retail": bool(match),
        "provenance": "the image " + words if match else a.provenance,
        "disc": {"bytes": image[0], "crc32": image[1], "sha1": image[2]},
        "archive": {"index": INDEX, "data": DATA, "header": header, "entries": len(entries), "data_bytes": data_bytes},
    }
    # one entry a line: [name, size]
    head = json.dumps(table, indent=1, ensure_ascii=True)
    rows = ",\n".join("  " + json.dumps([n, s], ensure_ascii=True) for n, _, s in entries)
    text = head[:-2] + ",\n \"entries\": [\n" + rows + "\n ]\n}\n"
    json.loads(text)   # the file reads back
    out = a.out or os.path.join(manifest.ROOT, "launcher", "discs", "archives", serial + ".json")
    os.makedirs(os.path.dirname(out), exist_ok=True)
    with open(out, "w", newline="\n") as f:
        f.write(text)
    print("archive_index: %s: %s %s, %d entries, %s" % (os.path.relpath(out, manifest.ROOT), serial, region, len(entries),
                                                        "retail" if match else "not retail: " + a.provenance))
    return 0


if __name__ == "__main__":
    sys.exit(main())
