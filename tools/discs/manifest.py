#!/usr/bin/env python3
"""tools/discs/manifest.py -- a disc's FILE LIST: every file's path, size and sha256, the program SYSTEM.CNF boots and
its ELF header facts, the whole image's size / CRC-32 / SHA-1, and where the list came from, as JSON
(launcher/discs/<id>.json). Metadata only -- names, sizes, offsets and hashes, never a byte of the files -- so the list
may live in the repository (tools/ci/asset_scan.py passes over it).

A list is labelled by PROVENANCE:
  --label retail      only when the whole image matches a known retail dump in launcher/discs/redump.json (size, CRC-32
                      and SHA-1 as redump.org publishes them). The list then says "retail": true and the launcher and the
                      game may name the files a modified disc changes against it. Refused (exit 2) for any other image.
  --label this-disc   (the default) a list of THIS disc's files, whatever the disc is: "retail": false and the
                      provenance in words (--provenance). Its `core` rows still name the title's core files; nothing is
                      ever called different from it.

  tools/discs/manifest.py IMAGE --id pal [--label this-disc|retail] [--provenance TEXT] [--title "Crash Twinsanity"]
                          [--core PATH ...] [--redump launcher/discs/redump.json] [--out launcher/discs/pal.json]
  tools/discs/manifest.py IMAGE --check launcher/discs/pal.json   # compare a disc with a list (exit 0 = every file matches)
  tools/discs/manifest.py IMAGE --identify                        # the image against the known dumps only (exit 0 = retail)

IMAGE: a plain 2048-byte-sector .iso or a raw 2352-byte-sector .bin (Mode 1 / Mode 2 Form 1), read only. The whole-image
hashes are over the logical 2048-byte sectors (the file itself for a .iso: what a DVD dump is).
"""
import argparse
import hashlib
import json
import os
import re
import struct
import sys
import zlib

SECTOR = 2048
RAW = 2352
DEFAULT_CORE = ["SYSTEM.CNF", "CRASH6/CRASH.BH", "CRASH6/CRASH.BD", "CRASH6/MUSIC.MH", "CRASH6/MUSIC.MB"]
SERIAL_RE = re.compile(r"^[A-Z]{4}_\d{3}\.\d{2}$")
REGIONS = {"SLES": "PAL", "SCES": "PAL", "SCED": "PAL", "SLED": "PAL", "SLUS": "NTSC-U", "SCUS": "NTSC-U",
           "SLPM": "NTSC-J", "SLPS": "NTSC-J", "SCPS": "NTSC-J", "SCAJ": "NTSC-J", "SLAJ": "NTSC-J"}
ROOT = os.path.abspath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
DEFAULT_REDUMP = os.path.join(ROOT, "launcher", "discs", "redump.json")


class Image:
    """The volume's logical 2048-byte sectors, whichever layout the file has."""

    def __init__(self, path):
        self.f = open(path, "rb")
        self.f.seek(0, os.SEEK_END)
        self.file_bytes = self.f.tell()
        self.f.seek(0)
        head = self.f.read(16)
        self.stride, self.user = SECTOR, 0
        if head[:12] == b"\x00" + b"\xff" * 10 + b"\x00":
            self.stride = RAW
            self.user = 24 if head[15] == 2 else 16
        self.logical_bytes = self.file_bytes if self.stride == SECTOR else (self.file_bytes // RAW) * SECTOR
        pvd = self.read(16 * SECTOR, SECTOR)
        if pvd[1:6] != b"CD001" or pvd[0] != 1:
            raise SystemExit("manifest: %s is not an ISO 9660 volume (no primary volume descriptor at sector 16)" % path)
        self.volume_id = pvd[40:72].decode("latin1").strip()
        self.sectors = struct.unpack_from("<I", pvd, 80)[0]
        self.root = (struct.unpack_from("<I", pvd, 158)[0], struct.unpack_from("<I", pvd, 166)[0])

    def read(self, offset, size):
        if self.stride == SECTOR:
            self.f.seek(offset)
            return self.f.read(size)
        out = bytearray()
        while size > 0:
            lba, within = divmod(offset, SECTOR)
            n = min(size, SECTOR - within)
            self.f.seek(lba * RAW + self.user + within)
            chunk = self.f.read(n)
            if not chunk:
                break
            out += chunk
            offset += n
            size -= n
        return bytes(out)

    def records(self, lba, size):
        data = self.read(lba * SECTOR, size)
        off = 0
        while off < len(data):
            n = data[off]
            if n == 0:
                off = (off // SECTOR + 1) * SECTOR
                continue
            r = data[off:off + n]
            ident = r[33:33 + r[32]]
            if ident not in (b"\x00", b"\x01"):
                yield (ident.decode("latin1").split(";")[0].upper(), struct.unpack_from("<I", r, 2)[0],
                       struct.unpack_from("<I", r, 10)[0], bool(r[25] & 2))
            off += n

    def files(self):
        out = []

        def walk(lba, size, prefix, depth):
            if depth > 16:
                return
            for name, ext, n, is_dir in self.records(lba, size):
                if is_dir:
                    walk(ext, n, prefix + name + "/", depth + 1)
                else:
                    out.append((prefix + name, ext, n))
        walk(self.root[0], self.root[1], "", 0)
        return out

    def sha256(self, lba, size):
        h = hashlib.sha256()
        pos = 0
        while pos < size:
            n = min(8 << 20, size - pos)
            chunk = self.read(lba * SECTOR + pos, n)
            if len(chunk) != n:
                raise SystemExit("manifest: the image ends inside a file (at byte %d of %d)" % (pos + len(chunk), size))
            h.update(chunk)
            pos += n
        return h.hexdigest()

    def image_hashes(self):
        """(bytes, crc32 hex, sha1 hex) of the logical image -- the file of a .iso, the user data of a raw .bin."""
        crc, sha, pos = 0, hashlib.sha1(), 0
        while pos < self.logical_bytes:
            n = min(16 << 20, self.logical_bytes - pos)
            chunk = self.read(pos, n)
            if len(chunk) != n:
                raise SystemExit("manifest: the image reads short at byte %d" % (pos + len(chunk)))
            crc = zlib.crc32(chunk, crc)
            sha.update(chunk)
            pos += n
        return self.logical_bytes, "%08x" % (crc & 0xFFFFFFFF), sha.hexdigest()


def system_cnf(img, files):
    for path, lba, size in files:
        if path == "SYSTEM.CNF":
            text = img.read(lba * SECTOR, min(size, 4096)).decode("latin1")
            out = {}
            for line in text.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    out[k.strip().upper()] = v.strip()
            return out
    return {}


def program_of(boot):
    name = re.split(r"[\\/:]", boot)[-1].split(";")[0].upper()
    return name


def describe(img, path_in):
    files = img.files()
    cnf = system_cnf(img, files)
    exe = program_of(cnf.get("BOOT2", "")) if cnf.get("BOOT2") else ""
    if not exe:
        exe = next((p for p, _, _ in files if "/" not in p and SERIAL_RE.match(p)), "")
    elf = {}
    for path, lba, size in files:
        if path == exe:
            h = img.read(lba * SECTOR, 52)
            if h[:4] == b"\x7fELF" and h[4] == 1 and h[5] == 1:
                elf = {"entry": "0x%08x" % struct.unpack_from("<I", h, 24)[0], "machine": struct.unpack_from("<H", h, 18)[0],
                       "bytes": size}
    return files, cnf, exe, elf


def serial_of(exe):
    return exe.replace("_", "-").replace(".", "") if SERIAL_RE.match(exe) else ""


def thousands(n):
    return "{:,}".format(n)


def against_redump(redump_path, serial, image):
    """The image (bytes, crc32, sha1) against the known dumps of `serial`: (match row or None, compared rows, words)."""
    with open(redump_path) as f:
        table = json.load(f)
    if table.get("kind") != "redump":
        raise SystemExit("manifest: %s is not a table of known dumps" % redump_path)
    rows = [d for d in table["dumps"] if d["serial"].replace(" ", "-") == serial and d.get("game", True)]
    bytes_, crc, sha1 = image
    for d in rows:
        if d["bytes"] == bytes_ and d["crc32"].lower() == crc and d["sha1"].lower() == sha1:
            return d, rows, "matches Redump disc %d (%s v%s): size, CRC-32 and SHA-1" % (d["id"], serial, d["version"])
    if not rows:
        return None, rows, "no known retail dump of %s is in %s" % (serial or "this program", os.path.relpath(redump_path, ROOT))
    near = min(rows, key=lambda d: abs(d["bytes"] - bytes_))
    if near["bytes"] != bytes_:
        why = "%s bytes vs %s of Redump disc %d (%s v%s)" % (thousands(bytes_), thousands(near["bytes"]), near["id"], serial, near["version"])
    else:
        why = "the size of Redump disc %d (%s v%s), but CRC-32 %s / SHA-1 %s differ" % (near["id"], serial, near["version"], crc, sha1)
    return None, rows, "matches no known retail dump: " + why


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("image")
    ap.add_argument("--id", help="the list's id (pal, ntsc-u, ...)")
    ap.add_argument("--title", default="Crash Twinsanity")
    ap.add_argument("--core", nargs="*", default=DEFAULT_CORE)
    ap.add_argument("--label", choices=["this-disc", "retail"], default="this-disc",
                    help="retail: only when the image matches a known retail dump; this-disc (default): a list of this disc's files")
    ap.add_argument("--provenance", default="", help="where the disc came from, in words (this-disc lists)")
    ap.add_argument("--redump", default=DEFAULT_REDUMP, help="the table of known retail dumps")
    ap.add_argument("--out", default="")
    ap.add_argument("--check", default="", help="compare the image with this list instead of writing one")
    ap.add_argument("--identify", action="store_true", help="only compare the whole image with the known dumps")
    a = ap.parse_args()
    img = Image(a.image)
    files, cnf, exe, elf = describe(img, a.image)
    serial = serial_of(exe)
    if a.identify:
        match, rows, words = against_redump(a.redump, serial, img.image_hashes())
        print("manifest --identify: %s %s -- %s" % (exe or "(no program)", serial, ("RETAIL: " if match else "") + words))
        return 0 if match else 1
    if a.check:
        with open(a.check) as f:
            want = json.load(f)
        have = {p: (lba, n) for p, lba, n in files}
        diffs = []
        for row in want["files"]:
            if row["path"] not in have:
                diffs.append("missing " + row["path"])
                continue
            lba, n = have.pop(row["path"])
            if n != row["bytes"] or img.sha256(lba, n) != row["sha256"]:
                diffs.append("differs " + row["path"])
        diffs += ["added " + p for p in sorted(have)]
        kind = "the RETAIL file list" if want.get("retail") else "a list of this disc's files (provenance: %s)" % want.get("provenance", "unknown")
        print("manifest --check: %s is %s -- %s" % (a.check, kind, "every file matches" if not diffs else "%d differences" % len(diffs)))
        for d in diffs:
            print("  " + d)
        return 0 if not diffs else 1
    if not a.id:
        ap.error("--id is required to write a list")
    image = img.image_hashes()
    match, rows, words = against_redump(a.redump, serial, image)
    print("manifest: the image (%s bytes, crc32 %s, sha1 %s) %s" % (thousands(image[0]), image[1], image[2], words))
    if a.label == "retail" and not match:
        print("manifest: REFUSED --label retail: the image %s. A list made from it is a list of this disc's files "
              "(--label this-disc), never a retail list." % words, file=sys.stderr)
        return 2
    rows_out = []
    for path, lba, size in sorted(files, key=lambda r: r[1]):
        rows_out.append({"path": path, "bytes": size, "sha256": img.sha256(lba, size), "core": path in a.core or path == exe})
    missing_core = [c for c in a.core if c not in {r["path"] for r in rows_out}]
    if missing_core:
        raise SystemExit("manifest: the core files %s are not on this disc" % ", ".join(missing_core))
    if match:
        provenance = "the retail dump: Redump disc %d (%s v%s), matched by size, CRC-32 and SHA-1" % (match["id"], serial, match["version"])
    else:
        provenance = a.provenance or "a %s image that %s" % (serial or exe, words)
    prefix = exe[:4] if SERIAL_RE.match(exe) else ""
    doc = {
        "schema": 2,
        "kind": "files",
        "id": a.id,
        "title": a.title,
        "region": REGIONS.get(prefix, "unknown"),
        "serial": serial,
        "executable": exe,
        "volume_id": img.volume_id,
        "sectors": img.sectors,
        "retail": bool(match),
        "provenance": provenance,
        "image": {"bytes": image[0], "crc32": image[1], "sha1": image[2],
                  "redump": {"compared": [d["id"] for d in rows], "match": match["id"] if match else None, "verdict": words}},
        "system_cnf": {"boot2": cnf.get("BOOT2", ""), "ver": cnf.get("VER", ""), "vmode": cnf.get("VMODE", "")},
        "elf": elf,
        "made_by": "tools/discs/manifest.py --label %s: names, sizes and hashes only (no byte of any file)" % ("retail" if match else "this-disc"),
        "files": rows_out,
    }
    text = json.dumps(doc, indent=2) + "\n"
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)), exist_ok=True)
        with open(a.out, "w") as f:
            f.write(text)
        print("manifest: %s -- %s %s (%s), %d files, %d sectors, volume %s, %s" % (a.out, doc["region"], doc["serial"], exe,
              len(rows_out), img.sectors, img.volume_id, "RETAIL list" if match else "a list of this disc's files (not retail)"))
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
