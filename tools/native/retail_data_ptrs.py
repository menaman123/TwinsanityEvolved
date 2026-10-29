#!/usr/bin/env python3
"""retail_data_ptrs.py -- the PAL executable's data words that hold DATA addresses, for the Vita's native build.

The native build (CMake TE_VITA_NATIVE, runner/te_native.h) keeps the game's retail address space in te_guest_block: a
retail address X lives at te_guest_block + X. The retail data image the runner loads from the disc holds addresses of its
own data (string tables, vtables' owners, list heads, the C library's break): those words must get the block's base added
at load, or the game would follow them into memory that is not its own. This tool makes the COMMITTED list of those words
(tools/native/retail_data_ptrs.tsv: word addresses, the class, the target's symbol name -- no bytes of the disc) and turns
it into the C array the runner links.

  scan ISO SYMBOL_ADDRS --out retail_data_ptrs.tsv    the disc's SLES_525.68 (read in memory only) -> the list
  c TSV --out retail_data_ptrs.c                     the list -> `te_retail_data_pointer_words[]` (the build does this)

The rule (every word of .data .rodata .sdata, 0x2E6F00..0x30A460, as the code-pointer patch reads them):
  * a value in the image [0x2D9D90, 0x3DB200] that is a data symbol's START (symbol_addrs.txt, not type:func): a pointer
    (409 words on the PAL disc; the image's end 0x3DB200 is the break word g_HeapEnd's start value);
  * a value INSIDE a symbol: a pointer only when reviewed (REVIEWED below, 36 words: GlobalLanguagesArray's 34 pointers into
    its string tables, G_GameController_00309914's two list-head self pointers). The other 28 such words on the PAL disc
    are not pointers: short strings ("...", "= 1", "l01".."l12": bytes 0x2E 0x2E 0x2E 0x00 read as a word), packed 16-bit
    pairs (G_MicroCode_1_InstructionAmt's table) and a MIPS instruction inside the exception handler's copy (0x2EBB50).
The runner checks each listed word's value at load (an address in the image, or it is left alone and counted) and counts
the image-range words the list does not name, so another executable's data shows in the start-up line.
"""
import argparse
import re
import struct
import sys
from bisect import bisect_right
from pathlib import Path

SECTOR = 2048
IMG, IMG_END = 0x2D9D90, 0x3DB200
SECTIONS = [(".data", 0x2E6F00, 0x2EC3D2), (".rodata", 0x2EC400, 0x309868), (".sdata", 0x309880, 0x30A460)]
# Interior pointers reviewed by hand (word address -> what it is), PAL SLES-52568
REVIEWED = {a: "GlobalLanguagesArray: a language's name in its string table" for a in
            list(range(0x2E6F68, 0x2E6FD0 + 1, 8)) + [0x2E70F4] + list(range(0x2E7100, 0x2E7190 + 1, 8))}
REVIEWED.update({0x309948: "G_GameController_00309914: a list head pointing at itself",
                 0x30994C: "G_GameController_00309914: a list head pointing at itself"})


def iso_find(f, name):
    f.seek(16 * SECTOR)
    pvd = f.read(SECTOR)
    root = pvd[156:156 + 34]
    lba, size = struct.unpack_from("<I", root, 2)[0], struct.unpack_from("<I", root, 10)[0]
    f.seek(lba * SECTOR)
    data = f.read(size)
    i = 0
    while i < len(data):
        n = data[i]
        if n == 0:
            i = (i // SECTOR + 1) * SECTOR
            continue
        rec = data[i:i + n]
        flen = rec[32]
        fname = rec[33:33 + flen].decode("ascii", "replace").split(";")[0]
        if fname.upper() == name.upper():
            return struct.unpack_from("<I", rec, 2)[0], struct.unpack_from("<I", rec, 10)[0]
        i += n
    raise SystemExit(f"retail_data_ptrs: {name} is not in the disc image's root directory")


def read_symbols(path):
    syms = []
    for line in Path(path).read_text().splitlines():
        m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?", line)
        if m:
            rest = m.group(3) or ""
            ms = re.search(r"size:0x([0-9A-Fa-f]+)", rest)
            syms.append((int(m.group(2), 16), int(ms.group(1), 16) if ms else 0, "type:func" in rest, m.group(1)))
    syms.sort()
    return syms


def scan(a):
    with open(a.iso, "rb") as f:
        lba, size = iso_find(f, "SLES_525.68")
        f.seek(lba * SECTOR)
        elf = f.read(size)
    phoff, = struct.unpack_from("<I", elf, 28)
    phentsize, phnum = struct.unpack_from("<HH", elf, 42)
    segs = []
    for k in range(phnum):
        p_type, p_offset, p_vaddr, _, p_filesz, _ = struct.unpack_from("<IIIIII", elf, phoff + k * phentsize)
        if p_type == 1:
            segs.append((p_vaddr, p_filesz, p_offset))

    def word(addr):
        for va, fs, off in segs:
            if va <= addr and addr + 4 <= va + fs:
                return struct.unpack_from("<I", elf, off + addr - va)[0]
        return None

    syms = read_symbols(a.symbols)
    starts = [s[0] for s in syms]
    data_start = {}
    for s in syms:
        if not s[2]:
            data_start.setdefault(s[0], s[3])
    rows, skipped = [], []
    for _, lo, hi in SECTIONS:
        for addr in range(lo, hi - 3, 4):
            v = word(addr)
            if v is None or not (IMG <= v <= IMG_END):
                continue
            if v in data_start:
                rows.append((addr, "symbol", data_start[v]))
                continue
            i = bisect_right(starts, v) - 1
            target = f"{syms[i][3]}+0x{v - syms[i][0]:X}" if i >= 0 else f"0x{v:X}"
            if addr in REVIEWED:
                rows.append((addr, "interior", target))
            else:
                skipped.append(addr)
    missing = sorted(set(REVIEWED) - {r[0] for r in rows})
    if missing:
        raise SystemExit("retail_data_ptrs: reviewed words that are no image address on this disc: "
                         + ", ".join(f"0x{m:X}" for m in missing))
    L = ["# tools/native/retail_data_ptrs.tsv -- GENERATED by tools/native/retail_data_ptrs.py scan (PAL SLES_525.68):",
         "# the words of the retail data image (.data .rodata .sdata) that hold DATA addresses -- the Vita's native build adds",
         "# the guest block's base to each at load (runner/te_native.cpp). Word addresses and symbol names only.",
         f"# {len(rows)} words: {sum(1 for r in rows if r[1] == 'symbol')} at a data symbol's start, "
         f"{sum(1 for r in rows if r[1] == 'interior')} reviewed interior pointers; {len(skipped)} image-range words are not pointers.",
         "# word\tclass\ttarget"]
    L += [f"0x{addr:08X}\t{cls}\t{target}" for addr, cls, target in rows]
    Path(a.out).write_text("\n".join(L) + "\n")
    print(f"retail_data_ptrs: {len(rows)} pointer words ({len(skipped)} image-range words left out) -> {a.out}")
    return 0


def to_c(a):
    words = []
    for line in Path(a.tsv).read_text().splitlines():
        if not line.strip() or line.startswith("#"):
            continue
        words.append(int(line.split("\t")[0], 16))
    if words != sorted(set(words)):
        raise SystemExit("retail_data_ptrs: the list is not sorted and unique")
    L = ["/* GENERATED by tools/native/retail_data_ptrs.py c from tools/native/retail_data_ptrs.tsv -- DO NOT EDIT. */",
         "#include <stdint.h>", f"const uint32_t te_retail_data_pointer_words[{len(words)}] = {{"]
    for i in range(0, len(words), 8):
        L.append("    " + ", ".join(f"0x{w:08X}u" for w in words[i:i + 8]) + ",")
    L += ["};", f"const uint32_t te_retail_data_pointer_word_count = {len(words)}u;", ""]
    Path(a.out).write_text("\n".join(L))
    return 0


def main():
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    s = sub.add_parser("scan")
    s.add_argument("iso")
    s.add_argument("symbols")
    s.add_argument("--out", required=True)
    c = sub.add_parser("c")
    c.add_argument("tsv")
    c.add_argument("--out", required=True)
    a = ap.parse_args()
    return scan(a) if a.cmd == "scan" else to_c(a)


if __name__ == "__main__":
    sys.exit(main())
