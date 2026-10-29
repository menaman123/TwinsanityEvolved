#!/usr/bin/env python3
"""apply_overlay_to_iso.py -- a copy of the disc image with a mod folder composed INTO it, as a patcher program would write
a modified image (TEST ONLY: the mod import judge makes a "patched ISO" from a fixture overlay with it, then checks that
the importer turns that image back into an equivalent overlay). Never run on anything but a temporary copy; the output is
deleted by the caller (2.6 GB for this game's disc).

The composition is the overlay's: disc/<DISC PATH> whole files and disc/<DISC PATH>.ranges
recipes (base <offset> <length> = the disc's own file, file <relpath> <offset> <length> = the mod's bytes, zero <length>).
Only IN-PLACE compositions are written -- every composed file fits the sectors its disc file occupies (the file's
directory record gets the new size); an added file, or a grown one past its sectors, would need the volume relaid and is
refused (exit 2). Layers do not stack here: one mod folder.

    apply_overlay_to_iso.py --iso DISC --mod MODDIR --out OUT.iso

Also a module: compose(iso, mod_dir, disc_path) -> (size, iterator of byte chunks) for the judges' own recomposition.
"""
import argparse
import hashlib
import os
import shutil
import struct
import sys

SECTOR = 2048


class Disc:
    """A 2048-byte-sector ISO 9660 image: its files with where their directory records are."""

    def __init__(self, path):
        self.path = path
        self.f = open(path, "rb")
        pvd = self.sector(16)
        if pvd[0] != 1 or pvd[1:6] != b"CD001":
            raise ValueError("%s: no ISO 9660 primary volume descriptor (a raw .bin image is not supported here)" % path)
        root = pvd[156:156 + 34]
        self.files = {}   # "CRASH6/CRASH.BD" -> {lba, size, rec: absolute byte offset of the directory record}
        self._walk(struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0], "", 0)

    def sector(self, lba, count=1):
        self.f.seek(lba * SECTOR)
        return self.f.read(count * SECTOR)

    def _walk(self, lba, size, prefix, depth):
        if depth > 16:
            raise ValueError("directories nested too deeply")
        data = self.sector(lba, (size + SECTOR - 1) // SECTOR)
        at = 0
        while at < size:
            n = data[at]
            if n == 0:
                at = (at // SECTOR + 1) * SECTOR
                continue
            rec_lba = struct.unpack_from("<I", data, at + 2)[0]
            rec_size = struct.unpack_from("<I", data, at + 10)[0]
            flags = data[at + 25]
            name = data[at + 33:at + 33 + data[at + 32]].decode("latin-1")
            if name not in ("\x00", "\x01"):
                clean = name.split(";")[0].upper()
                if flags & 2:
                    self._walk(rec_lba, rec_size, prefix + clean + "/", depth + 1)
                else:
                    self.files[prefix + clean] = {"lba": rec_lba, "size": rec_size, "rec": lba * SECTOR + at}
            at += n

    def read(self, path, offset, size):
        e = self.files[path]
        if offset + size > e["size"]:
            raise ValueError("%s: %d+%d is past its end" % (path, offset, size))
        self.f.seek(e["lba"] * SECTOR + offset)
        return self.f.read(size)


def recipe(text):
    """The segments of a .ranges recipe: ("base", off, len) | ("file", relpath, off, len) | ("zero", len)."""
    segs = []
    for line in text.splitlines():
        cut = [k for k in range(len(line)) if line[k] == "#" and (k == 0 or line[k - 1] in " \t")]
        if cut:
            line = line[:cut[0]]
        tok = line.split()
        if not tok:
            continue
        num = lambda s: int(s, 16) if s.lower().startswith("0x") else int(s)
        op = tok[0].lower()
        if op == "base" and len(tok) == 3:
            segs.append(("base", num(tok[1]), num(tok[2])))
        elif op == "file" and len(tok) >= 4:
            rel = line[line.index(tok[1], line.index(tok[0]) + len(tok[0])):].rsplit(None, 2)[0].strip()
            segs.append(("file", rel, num(tok[-2]), num(tok[-1])))
        elif op == "zero" and len(tok) == 2:
            segs.append(("zero", num(tok[1])))
        else:
            raise ValueError("a recipe line this tool does not read: %r" % line)
    return segs


def mod_files(mod_dir):
    """{disc path: (host path, is_recipe)} of a mod folder's disc/ tree."""
    out = {}
    root = os.path.join(mod_dir, "disc")
    for dirpath, dirs, names in os.walk(root):
        dirs[:] = [d for d in dirs if not d.startswith(".")]
        for name in names:
            if name.startswith(".") or name.lower() in ("thumbs.db", "desktop.ini"):
                continue
            host = os.path.join(dirpath, name)
            rel = os.path.relpath(host, root).replace(os.sep, "/")
            is_recipe = rel.lower().endswith(".ranges")
            disc = (rel[:-7] if is_recipe else rel).upper()
            out[disc] = (host, is_recipe)
    return out


def compose(disc, mod_dir, path, chunk=1 << 22):
    """(size, chunks) of the mod's version of `path` over `disc` (a Disc); (None, None) when the mod does not touch it."""
    files = mod_files(mod_dir)
    if path not in files:
        return None, None
    host, is_recipe = files[path]
    if not is_recipe:
        size = os.path.getsize(host)

        def whole():
            with open(host, "rb") as f:
                while True:
                    b = f.read(chunk)
                    if not b:
                        return
                    yield b
        return size, whole()
    with open(host, encoding="utf-8") as f:
        segs = recipe(f.read())
    size = sum(s[-1] for s in segs)

    def pieces():
        for s in segs:
            if s[0] == "base":
                off, n = s[1], s[2]
                while n:
                    k = min(n, chunk)
                    yield disc.read(path, off, k)
                    off += k
                    n -= k
            elif s[0] == "file":
                with open(os.path.join(mod_dir, s[1]), "rb") as f:
                    f.seek(s[2])
                    n = s[3]
                    while n:
                        b = f.read(min(n, chunk))
                        if not b:
                            raise ValueError("%s ends early" % s[1])
                        yield b
                        n -= len(b)
            else:
                n = s[1]
                while n:
                    k = min(n, chunk)
                    yield bytes(k)
                    n -= k
    return size, pieces()


def composed_sha256(disc, mod_dir, path):
    size, chunks = compose(disc, mod_dir, path)
    if size is None:
        return None
    h = hashlib.sha256()
    for b in chunks:
        h.update(b)
    return h.hexdigest()


def disc_sha256(disc, path, chunk=1 << 22):
    e = disc.files[path]
    h = hashlib.sha256()
    off = 0
    while off < e["size"]:
        k = min(chunk, e["size"] - off)
        h.update(disc.read(path, off, k))
        off += k
    return h.hexdigest()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--mod", required=True)
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    try:
        disc = Disc(a.iso)
        files = mod_files(a.mod)
        if not files:
            raise ValueError("%s has no disc/ tree" % a.mod)
        plan = []
        for path in sorted(files):
            if path not in disc.files:
                raise ValueError("%s is not on the disc: an added file needs the volume relaid (not done here)" % path)
            size, _ = compose(disc, a.mod, path)
            e = disc.files[path]
            room = (e["size"] + SECTOR - 1) // SECTOR * SECTOR
            if size > room:
                raise ValueError("%s grows to %d bytes past its %d on the disc: not an in-place composition" % (path, size, room))
            plan.append((path, size))
    except (OSError, ValueError) as e:
        print("apply_overlay_to_iso: %s" % e, file=sys.stderr)
        return 2
    try:
        shutil.copyfile(a.iso, a.out)
        with open(a.out, "r+b") as out:
            for path, size in plan:
                e = disc.files[path]
                _, chunks = compose(disc, a.mod, path)
                out.seek(e["lba"] * SECTOR)
                written = 0
                for b in chunks:
                    out.write(b)
                    written += len(b)
                pad = (e["size"] + SECTOR - 1) // SECTOR * SECTOR - written
                out.write(bytes(max(0, pad)))
                if size != e["size"]:   # the directory record's data length, both byte orders
                    out.seek(e["rec"] + 10)
                    out.write(struct.pack("<I", size) + struct.pack(">I", size))
                print("apply_overlay_to_iso: %s: %d bytes at lba %d" % (path, size, e["lba"]))
    except (OSError, ValueError) as e:
        print("apply_overlay_to_iso: %s" % e, file=sys.stderr)
        try:
            os.remove(a.out)
        except OSError:
            pass
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
