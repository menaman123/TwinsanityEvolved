#!/usr/bin/env python3
"""retail_ptrs.py ISO SYMBOL_ADDRS -- the retail data image's words that hold DATA addresses (the native build must
relocate them: guest X -> block + X) and CODE addresses (patched today to table indices, natively to function addresses),
classified by how sure a scan can be. Reads the disc's ELF in memory only; prints counts, never data."""
import re
import struct
import sys
from bisect import bisect_right

iso_path, syms_path = sys.argv[1:3]
SECTOR = 2048


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
    raise SystemExit(f"{name} not in the ISO root")


with open(iso_path, "rb") as f:
    lba, size = iso_find(f, "SLES_525.68")
    f.seek(lba * SECTOR)
    elf = f.read(size)
phoff, = struct.unpack_from("<I", elf, 28)
phentsize, phnum = struct.unpack_from("<HH", elf, 42)
mem = {}
for k in range(phnum):
    p_type, p_offset, p_vaddr, _, p_filesz, p_memsz = struct.unpack_from("<IIIIII", elf, phoff + k * phentsize)
    if p_type == 1:
        mem[(p_vaddr, p_vaddr + p_filesz)] = elf[p_offset:p_offset + p_filesz]


def word(a):
    for (lo, hi), b in mem.items():
        if lo <= a and a + 4 <= hi:
            return struct.unpack_from("<I", b, a - lo)[0]
    return None


syms = []
for line in open(syms_path):
    m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?", line)
    if m:
        rest = m.group(3) or ""
        ms = re.search(r"size:0x([0-9A-Fa-f]+)", rest)
        syms.append((int(m.group(2), 16), int(ms.group(1), 16) if ms else 0, "type:func" in rest, m.group(1)))
syms.sort()
starts = [s[0] for s in syms]
data_starts = {s[0] for s in syms if not s[2]}
func_starts = {s[0] for s in syms if s[2]}

IMG, IMG_END = 0x2D9D90, 0x3DB200
SECTIONS = [(".data", 0x2E6F00, 0x2EC3D2), (".rodata", 0x2EC400, 0x309868), (".sdata", 0x309880, 0x30A460)]
tot = {"words": 0, "zero": 0, "code_func_start": 0, "code_other": 0, "data_symbol_start": 0, "data_inside_sized": 0,
       "data_unknown": 0, "data_end_exact": 0}
per = {}
for name, lo, hi in SECTIONS:
    c = {k: 0 for k in tot}
    for a in range(lo, hi - 3, 4):
        v = word(a)
        if v is None:
            continue
        c["words"] += 1
        if v == 0:
            c["zero"] += 1
        elif 0x100000 <= v < IMG:
            c["code_func_start" if v in func_starts else "code_other"] += 1
        elif IMG <= v <= IMG_END:
            if v in data_starts:
                c["data_symbol_start"] += 1
            elif v == IMG_END:
                c["data_end_exact"] += 1
            else:
                i = bisect_right(starts, v) - 1
                if i >= 0 and syms[i][1] and syms[i][0] <= v < syms[i][0] + syms[i][1] and not syms[i][2]:
                    c["data_inside_sized"] += 1
                else:
                    c["data_unknown"] += 1
    per[name] = c
    for k in tot:
        tot[k] += c[k]
print("section   " + "  ".join(f"{k:>17}" for k in tot))
for name, c in per.items():
    print(f"{name:9} " + "  ".join(f"{c[k]:17d}" for k in tot))
print(f"{'total':9} " + "  ".join(f"{tot[k]:17d}" for k in tot))
