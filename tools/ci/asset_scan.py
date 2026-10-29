#!/usr/bin/env python3
"""tools/ci/asset_scan.py -- NO GAME DATA IN THE TREE (a release gate). Zero findings or the gate fails.

Scans every file of the port tree and the game/ tree by EXTENSION and by MAGIC: disc images (ISO9660 "CD001",
raw .bin/.cue), executables (ELF/IRX), archives (zip, 7z, rar, gzip, xz, zstd, tar), textures/images (PNG, JPEG,
GIF, BMP, DDS, TGA), audio (RIFF/WAVE, VAGp, Ogg, MP3/ID3), video (MPEG/PSS pack header, MP4 ftyp, AVI, MOV), PS2
saves/savestates (.ps2st/.p2s/.psu/.mcd). Excluded: .git, build directories, the fetched toolchain, __pycache__.

This is the Python port of the former asset_scan.sh, so no lane needs Git for Windows' bash.exe (the launcher's
MinGit ships none; a machine with WSL must not fall back to System32\bash.exe). The engine's own scan
(engine/tools/ci/asset_scan.py) is unchanged: it guards the engine's artifacts with title-specific byte windows.

EXCLUDED PATHS (an explicit list, the excluded() function): the engine/ tree is the vendored NativeEE engine with
its own test fixtures (the GS swizzle-table .bin, atlas dumps) and its own asset gate (engine/tools/ci/asset_scan.py,
run by the engine's workflows); it is NOT scanned by default -- its content is not part of the port's build output
-- and `--include-engine` reports it informationally. game/ IS scanned: it is the decompilation's source and must
hold no game data.

  tools/ci/asset_scan.py [--include-engine] [ROOT]
"""
import os
import re
import sys

EXT_RE = re.compile(
    r"\.(iso|bin|cue|img|mdf|nrg|elf|irx|erx|ps2st|p2s|psu|mcd|mc2|max|cbs|pss|vag|va[gp]|bd|bh|mb|mh|rm2|sm2|psf|"
    r"tga|dds|png|jpg|jpeg|gif|bmp|wav|mp3|ogg|flac|mp4|avi|mov|mkv|m2v|zip|7z|rar|tar|gz|tgz|xz|zst|bz2)$"
)
SCREENSHOT_RE = re.compile(r"^docs/launcher/screenshots/launcher-[a-z0-9-]+\.png$")


def excluded(rel, include_engine):
    """The find -prune list of the shell scan, relative to the scanned ROOT."""
    parts = rel.split("/")
    if parts[-1] in (".git", "__pycache__"):
        return True
    if len(parts) == 1 and parts[0].startswith("build"):
        return True
    if rel in ("tools/toolchain", "dist"):
        return True
    return not include_engine and rel == "engine"


def size_of(path):
    try:
        return os.path.getsize(path)
    except OSError:
        return 0


def read_at(path, offset, count):
    try:
        with open(path, "rb") as f:
            f.seek(offset)
            return f.read(count)
    except OSError:
        return b""


def magic_of(path):
    head = read_at(path, 0, 16)
    h = head.hex()
    if h.startswith("7f454c46"):
        return "ELF/IRX executable"
    if h.startswith("89504e470d0a1a0a"):
        return "PNG image"
    if h.startswith("ffd8ff"):
        return "JPEG image"
    if len(head) >= 6 and h.startswith("47494638") and head[4] in (0x37, 0x39) and head[5] == 0x61:
        return "GIF image"
    if h.startswith("424d"):
        pass  # BMP: too weak alone ("BM" prefix); the extension rule covers it
    elif h.startswith("44445320"):
        return "DDS texture"
    elif h.startswith(("504b0304", "504b0506")):
        return "ZIP archive"
    elif h.startswith("377abcaf271c"):
        return "7z archive"
    elif h.startswith("526172211a07"):
        return "RAR archive"
    elif h.startswith("1f8b"):
        return "gzip archive"
    elif h.startswith("fd377a585a00"):
        return "xz archive"
    elif h.startswith("28b52ffd"):
        return "zstd archive"
    elif h.startswith("56414770"):
        return "VAG audio"
    elif h.startswith("4f676753"):
        return "Ogg audio"
    elif h.startswith("494433"):
        return "MP3/ID3 audio"
    elif h.startswith(("000001ba", "000001b3")):
        return "MPEG/PSS video"
    elif h.startswith("52494646") and head[8:12] == b"WAVE":
        return "RIFF/WAVE audio"
    elif h.startswith("52494646") and head[8:12] == b"AVI ":
        return "RIFF/AVI video"
    if b"ftyp" in head[4:12]:
        return "MP4/MOV video"
    # ISO9660: "CD001" at byte 0x8001 (plain) -- only worth checking on files >= 64 KB
    if size_of(path) >= 65536 and read_at(path, 0x8001, 5) == b"CD001":
        return "ISO9660 disc image"
    if read_at(path, 257, 5) == b"ustar":
        return "tar archive"
    return ""


def main():
    include_engine = False
    roots = []
    for arg in sys.argv[1:]:
        if arg == "--include-engine":
            include_engine = True
        else:
            roots.append(arg)
    if len(roots) > 1:
        sys.exit("asset_scan: usage: tools/ci/asset_scan.py [--include-engine] [ROOT]")
    root = os.path.abspath(roots[0] if roots else os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))
    if not os.path.isdir(root):
        sys.exit("asset_scan: ROOT is not a directory: %s" % root)

    scanned = 0
    findings = 0

    def report(rel, reason):
        nonlocal findings
        findings += 1
        print("FINDING: %s  (%s)" % (rel, reason))

    for dirpath, dirnames, filenames in os.walk(root):
        rel_dir = os.path.relpath(dirpath, root).replace(os.sep, "/")
        dirnames[:] = sorted(
            d for d in dirnames
            if not excluded(d if rel_dir == "." else rel_dir + "/" + d, include_engine)
        )
        for name in sorted(filenames):
            rel = name if rel_dir == "." else rel_dir + "/" + name
            path = os.path.join(dirpath, name)
            scanned += 1
            lower = rel.lower()
            # The ONE picture exemption: screenshots of the launcher's own window for README
            # (docs/launcher/screenshots/launcher-*.png, under 2 MB). The launcher shows no disc content --
            # its card artwork is generated from the title id -- so these carry no game data; anything else
            # under that folder, or larger, is still a finding.
            if SCREENSHOT_RE.match(lower) and size_of(path) <= 2097152:
                continue
            if EXT_RE.search(lower):
                report(rel, "forbidden extension")
                continue
            if re.match(r"(SLUS_|SLES_|SCUS_|SCES_|IOPRP)", name):
                report(rel, "PS2 executable name")
                continue
            magic = magic_of(path)
            if magic:
                report(rel, "magic: %s" % magic)

    suffix = " (engine included)" if include_engine else ""
    print("asset_scan: scanned %d files under %s%s, findings: %d" % (scanned, root, suffix, findings))
    return 1 if findings else 0


if __name__ == "__main__":
    sys.exit(main())
