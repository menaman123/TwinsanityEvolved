#!/usr/bin/env python3
"""tools/discs/elf_region_map.py -- the ADDRESS MAP that lets the port run another region's executable.

The port runs the decompilation's C++, compiled for the PAL executable's addresses: it reads the retail DATA image
(.vutext .data .vudata .rodata .sdata, 0x2D9D90..0x30A480 on SLES_525.68; .sbss/.bss zero) from the disc and addresses
its variables at their PAL addresses. Another release (NTSC-U SLUS_209.09, NTSC-J SLPM_658.01) is the same program linked
again from slightly different sources: the same objects at other addresses, a few objects moved between sections, a few
added or dropped, some tables with other contents. Its data image is laid out at the PAL addresses OBJECT BY OBJECT
through the map this tool makes, once, from the two executables (schema 2):

  1 CODE     the two .text sections aligned instruction by instruction (each instruction a token with its relocated
             fields masked: jump targets, lui immediates, the offsets of address-forming instructions; difflib's matching
             blocks). Every aligned instruction pair translates a code address (function starts and the case labels of
             the jump tables alike), and every aligned pair of address-forming instructions (lui + addiu/ori/load/store,
             gp-relative) is a VOTE: this PAL data address is that one in the other executable -- the code itself says
             where each variable went.
  2 DATA     each data section aligned by content: a word's token is its value, or for a code pointer the PAL function
             it is (through 1), for a data pointer to a string the string, for another data pointer a wildcard; equal
             blocks, gaps of the same length (the same place, other contents) and the first words of an unequal gap.
  3 OBJECTS  every PAL data object (game/symbol_addrs.txt's data symbols, the addresses the PAL code forms, the targets
             of the PAL data pointers) gets its counterpart: by content (2) agreeing with the code (1) or not, by the
             code alone for an object the content alignment did not place (moved: GlobalLanguagesArray .data -> .sdata),
             by its neighbours, or none. A moved object is aligned on its own (2 over the two objects' extents).
  4 BSS      .sbss/.bss placed by the section starts, every PAL bss object checked against the code's votes.
  5 POINTERS every word of the laid-out image that is a pointer in the other executable is translated: a code pointer
             through 1 (a function the PAL code does not have: the PAL slot's own function, the PAL-shaped default), a
             data pointer through the objects (an object only the other executable has: placed into the PAL-only
             object the PAL word at that place pointed to -- the same field's target -- when it fits).
  6 FILLS    a PAL object no byte of the other executable reaches keeps its PAL code pointers (addresses of the port's
             own functions: a vtable of a class only the PAL program has stays callable); everything else is zero.

The map holds ADDRESSES ONLY -- no byte of either executable: segments, bss ranges, the pointer words to translate, the
code and data address pairs, the fills (PAL function addresses), every PAL data symbol's counterpart with its evidence
and sizes, and the census. It may live in the repository (launcher/discs/region_map_<serial>.json) and serve every
player of that disc. It is made by someone who has BOTH discs:

  elf_region_map.py map --pal-iso PAL.iso --other-iso NTSC.iso --out launcher/discs/region_map_SLUS-20909.json
                        [--pal-elf FILE] [--other-elf FILE] [--symbols game/symbol_addrs.txt]
                        [--engine-map engine/.../pal_ntsc_map.tsv] [--data-names engine/.../data_symbols.tsv]
  elf_region_map.py shift --pal-iso PAL.iso --shift 0x1000 --out /tmp/shifted.elf   # the SYNTHETIC test executable
  elf_region_map.py verify --map MAP --other-elf FILE --pal-elf FILE                 # apply the map, compare the images
  elf_region_map.py census --pal-iso PAL.iso                                         # what the port reads
  elf_region_map.py selftest                                                         # synthetic ELFs, no game data

The same algorithm in C++ is engine/app/host/region_map_make.cpp (twinsanityevolved-modtool region-map); the two make the
same map (platform/nativeee/tests/test_region_map). Tests of the mechanism: PAL against PAL gives the identity map; PAL
against a synthetic executable made from it with every section moved by +0x1000 gives +0x1000 everywhere and rebuilds the
PAL data image byte for byte (tools/judges/region_map_judge.py); the real NTSC-U pair is the committed map.
"""
import argparse
import bisect
import collections
import difflib
import hashlib
import json
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)

# The PAL data image the port loads (runner/te_env.h RetailImage, tools/gen_retail_image.py SECTIONS)
PAL_IMAGE_START, PAL_BSS_START, PAL_IMAGE_END = 0x2D9D90, 0x30A480, 0x3DB200
DATA_SECTIONS = [".vutext", ".data", ".vudata", ".rodata", ".sdata"]
BSS_SECTIONS = [".sbss", ".bss"]
SHF_ALLOC = 2
SHT_NOBITS = 8
SCHEMA = 2
# the map's "made_by" (engine/app/host/region_map_make.h kRegionMapMadeBy: the two tools write the same file)
MADE_BY = ("tools/discs/elf_region_map.py map = twinsanityevolved-modtool region-map (schema 2): addresses and names only, "
           "no byte of either executable")
# the words the runner's code-pointer patch scans (runner/te_env.cpp PatchRetailCodePointers: .data .rodata .sdata)
PATCH_START, PATCH_END = 0x2E6F00, 0x30A460

# the tokens (64-bit: the C++ port uses the same values)
TAIL_BIT = 1 << 40          # the bytes after the last whole word of a section
POINTER_TOKEN = 1 << 41     # a data pointer whose target is not a string: a wildcard
CODE_TAG = 1 << 42          # a code pointer: the PAL code address it is (the PAL side: itself)
OTHER_CODE_TAG = 1 << 43    # a code pointer of the other executable with no PAL counterpart: its own address (matches nothing)
STRING_TAG = 1 << 44        # a data pointer to a string: the string's 40-bit FNV-1a hash
MASK40 = (1 << 40) - 1

# MIPS (the EE's R5900): the opcodes whose 16-bit immediate relocation changes, and how a data address is formed
OP_J, OP_JAL, OP_LUI, OP_ORI = 0x02, 0x03, 0x0F, 0x0D
ADDI_OPS = {0x08, 0x09, 0x18, 0x19}                        # addi addiu daddi daddiu
LOADS_GPR = {0x1A, 0x1B, 0x1E, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26, 0x27, 0x37}   # ldl ldr lq lb lh lwl lw lbu lhu lwr lwu ld
LOADS_OTHER = {0x31, 0x36}                                 # lwc1 lqc2 (a floating-point / vector register)
STORES = {0x1F, 0x28, 0x29, 0x2A, 0x2B, 0x2C, 0x2D, 0x2E, 0x39, 0x3E, 0x3F}   # sq sb sh swl sw sdl sdr swr swc1 sqc2 sd
MEMORY_OPS = LOADS_GPR | LOADS_OTHER | STORES | {0x33}     # + pref
IMM_ALU_DEST = {0x08, 0x09, 0x0A, 0x0B, 0x0C, 0x0D, 0x0E, 0x0F, 0x18, 0x19}
BRANCH_OPS = {0x01, 0x04, 0x05, 0x06, 0x07, 0x14, 0x15, 0x16, 0x17}   # regimm beq bne blez bgtz beql bnel blezl bgtzl
COP_OPS = {0x11, 0x12}                                     # cop1 / cop2: rs = 8 is a branch (bc1 / bc2)
LUI_WINDOW = 64                                            # a lui's high half is used within this many instructions
GP_OFFSET = 0x7FF0                                         # gp = the start of .sdata + 0x7FF0 (GNU ld's _gp)


# ------------------------------------------------------------------------------------------------------------- ELF
class Elf:
    """A 32-bit little-endian MIPS executable: the PT_LOAD segments and the named sections."""

    def __init__(self, data, name="?"):
        if data[:4] != b"\x7fELF" or data[4] != 1 or data[5] != 1:
            raise SystemExit("elf_region_map: %s is not a 32-bit little-endian ELF" % name)
        self.data, self.name = bytes(data), name
        self.entry = struct.unpack_from("<I", data, 24)[0]
        phoff, shoff = struct.unpack_from("<II", data, 28)
        phentsize, phnum, shentsize, shnum, shstrndx = struct.unpack_from("<HHHHH", data, 42)
        self.loads = []
        for i in range(phnum):
            p_type, p_offset, p_vaddr, _p_paddr, p_filesz, p_memsz = struct.unpack_from("<6I", data, phoff + i * phentsize)
            if p_type == 1:
                self.loads.append((p_vaddr, p_offset, p_filesz, p_memsz))
        if not self.loads:
            raise SystemExit("elf_region_map: %s has no PT_LOAD segment" % name)
        self.sections = {}
        if shoff and shnum:
            heads = [struct.unpack_from("<10I", data, shoff + i * shentsize) for i in range(shnum)]
            stroff = heads[shstrndx][4]
            for h in heads:
                nm = data[stroff + h[0]:data.index(b"\0", stroff + h[0])].decode("latin1")
                if h[2] & SHF_ALLOC and h[3]:
                    self.sections[nm] = {"addr": h[3], "size": h[5], "type": h[1], "offset": h[4]}
        self.lo = min(v for v, _, _, _ in self.loads)
        self.hi = max(v + m for v, _, _, m in self.loads)
        text = self.sections.get(".text")
        self.code = (text["addr"], text["addr"] + text["size"]) if text else (self.lo, self.lo)
        sdata = self.sections.get(".sdata")
        self.gp = (sdata["addr"] + GP_OFFSET) if sdata else 0
        firsts = [s["addr"] for n, s in self.sections.items() if n in DATA_SECTIONS]
        self.image_lo = min(firsts) if firsts else self.hi
        self.data_end = max((s["addr"] + s["size"] for n, s in self.sections.items() if n in DATA_SECTIONS), default=self.hi)

    def sha256(self):
        return hashlib.sha256(self.data).hexdigest()

    def read(self, vaddr, n):
        """n bytes at vaddr from the file-backed part of a PT_LOAD (zero past filesz, as the loader leaves them)."""
        out = bytearray(n)
        for v, off, fsz, msz in self.loads:
            lo, hi = max(vaddr, v), min(vaddr + n, v + fsz)
            if lo < hi:
                out[lo - vaddr:hi - vaddr] = self.data[off + lo - v:off + hi - v]
        return bytes(out)

    def word(self, vaddr):
        return struct.unpack("<I", self.read(vaddr, 4))[0]

    def pointer_like(self, w):
        return self.lo <= w < self.hi

    def in_code(self, w):
        return self.code[0] <= w < self.code[1]

    def in_image(self, w):
        """a data address of the image the port loads ([.vutext, the end of .bss]; the end itself too: one past the end)"""
        return self.image_lo <= w <= self.hi

    def cstring(self, a):
        """the printable NUL-terminated string at a (1..63 characters), else None"""
        b = self.read(a, 64)
        n = b.find(b"\0")
        if n <= 0:
            return None
        s = b[:n]
        if all(0x20 <= c < 0x7F or c in (9, 10, 13) for c in s):
            return s
        return None


def words(blob):
    n = len(blob) // 4
    out = list(struct.unpack_from("<%dI" % n, blob, 0)) if n else []
    if len(blob) % 4:
        out.append(int.from_bytes(blob[n * 4:], "little") | TAIL_BIT)   # the tail bytes: a token of their own
    return out


def fnv40(b):
    h = 0xCBF29CE484222325
    for c in b:
        h = ((h ^ c) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h & MASK40


def elf_from_iso(iso_path):
    import manifest
    img = manifest.Image(iso_path)
    files, cnf, exe, _elf = manifest.describe(img, iso_path)
    for path, lba, size in files:
        if path == exe:
            return exe, manifest.serial_of(exe), img.read(lba * manifest.SECTOR, size)
    raise SystemExit("elf_region_map: %s boots %s, which is not on the disc" % (iso_path, exe or "(nothing)"))


def load_program(elf_path, iso_path, what):
    if elf_path:
        with open(elf_path, "rb") as f:
            data = f.read()
        exe = os.path.basename(elf_path).upper()
        return exe, "", Elf(data, elf_path)
    if iso_path:
        exe, serial, data = elf_from_iso(iso_path)
        return exe, serial, Elf(data, exe)
    raise SystemExit("elf_region_map: the %s executable: --%s-elf FILE or --%s-iso DISC" % (what, what, what))


def read_symbols(path):
    """symbol_addrs.txt: {name: (addr, is_func)}."""
    out = {}
    if not path or not os.path.isfile(path):
        return out
    for line in open(path, encoding="utf-8", errors="replace"):
        m = re.match(r"(\w+)\s*=\s*0x([0-9A-Fa-f]+);\s*(?://\s*(.*))?", line)
        if m:
            rest = m.group(3) or ""
            out[m.group(1)] = (int(m.group(2), 16), "type:func" in rest)
    return out


def section_of(addr, ranges):
    for name, lo, hi in ranges:
        if lo <= addr < hi:
            return name
    return None


# ------------------------------------------------------------------------------------------------------------- alignment
def matching_blocks(a, b):
    """difflib.SequenceMatcher(None, a, b, autojunk=True).get_matching_blocks() as (i, j, n) (the C++ port: MatchingBlocks)"""
    return [(m.a, m.b, m.size) for m in difflib.SequenceMatcher(None, a, b, autojunk=True).get_matching_blocks()]


def aligned_runs(a, b, start_rule):
    """[(i, j, n, kind)] in order: 'equal' blocks; a gap of the same length on both sides 'replace' (the same place, other
    contents); an unequal gap's first min(m, n) tokens 'start' when start_rule(i, ma, j, mb) allows it (a field of other
    contents and length inside ONE object on each side: a shorter string, a record table with one more record)"""
    out, i, j = [], 0, 0
    for ma, mb, n in matching_blocks(a, b):
        gi, gj = ma - i, mb - j
        if gi > 0 and gj > 0:
            if gi == gj:
                out.append((i, j, gi, "replace"))
            elif start_rule and start_rule(i, ma, j, mb):
                out.append((i, j, min(gi, gj), "start"))
        if n:
            out.append((ma, mb, n, "equal"))
        i, j = ma + n, mb + n
    return out


# ------------------------------------------------------------------------------------------------------------- 1 code
def code_scan(elf, ws):
    """per instruction: the data address it forms (lui + addiu/daddiu/ori/load/store, or gp-relative; inside the image,
    else None), and its TOKEN -- the word with the fields relocation changes masked: a jump's target, a lui's immediate,
    the immediate of an instruction that forms an address in the executable from a lui or gp, a branch's offset (a branch
    to a shared tail spans whatever the other release inserted); a struct field's offset, a stack slot, a constant stay:
    they tell small functions apart"""
    refs, toks = [None] * len(ws), list(ws)
    hi = [None] * 32
    at = [0] * 32
    clear_after = False
    gp = elf.gp
    for k, w in enumerate(ws):
        op = w >> 26
        rs, rt = (w >> 21) & 31, (w >> 16) & 31
        imm = w & 0xFFFF
        simm = imm - 0x10000 if imm & 0x8000 else imm
        if op in (OP_J, OP_JAL):
            toks[k] = w & 0xFC000000
        elif op in BRANCH_OPS or (op in COP_OPS and rs == 8):
            toks[k] = w & 0xFFFF0000
        elif op == OP_LUI:
            hi[rt], at[rt] = imm << 16, k
            toks[k] = w & 0xFFFF0000
        else:
            v = None
            if op in ADDI_OPS or op in MEMORY_OPS:
                if rs == 28 and gp:
                    v = (gp + simm) & 0xFFFFFFFF
                elif rs and hi[rs] is not None and k - at[rs] <= LUI_WINDOW:
                    v = (hi[rs] + simm) & 0xFFFFFFFF
            elif op == OP_ORI and rs and rs != 28 and hi[rs] is not None and k - at[rs] <= LUI_WINDOW:
                v = hi[rs] | imm
            if v is not None and (elf.pointer_like(v) or v == elf.hi):
                toks[k] = w & 0xFFFF0000
                if elf.in_image(v):
                    refs[k] = v
            dest = None
            if op in IMM_ALU_DEST or op in LOADS_GPR:
                dest = rt
            elif op in (0x00, 0x1C):   # SPECIAL, MMI: rd
                dest = (w >> 11) & 31
            if dest:
                hi[dest] = None
        if clear_after:
            hi = [None] * 32
            clear_after = False
        if op == OP_JAL or (op == 0 and (w & 0x3F) == 0x09):   # a call: nothing survives its delay slot
            clear_after = True
    return refs, toks


def dest_of(w):
    """the general register an instruction writes (None for none)"""
    op = w >> 26
    if op in IMM_ALU_DEST or op in LOADS_GPR:
        return (w >> 16) & 31 or None
    if op in (0x00, 0x1C):
        return (w >> 11) & 31 or None
    return None


CALL_WINDOW = 32


def call_sites(elf, ws, refs):
    """{call target: [(site address, {argument register a0..a3: the address formed into it})]}: an argument register's
    last writer before a jal (its delay slot included; back to the previous call or return, at most CALL_WINDOW
    instructions), when that writer forms an address"""
    out = collections.defaultdict(list)
    lo = elf.code[0]
    for k, w in enumerate(ws):
        if w >> 26 != OP_JAL:
            continue
        t = ((lo + 4 * k) & 0xF0000000) | ((w & 0x3FFFFFF) << 2)
        window = [k + 1] if k + 1 < len(ws) else []
        x = k - 1
        while x >= 0 and k - x <= CALL_WINDOW:
            wx = ws[x]
            if wx >> 26 == OP_JAL or (wx >> 26 == 0 and (wx & 0x3F) in (0x08, 0x09)):   # a call, jr, jalr: the window ends
                break
            window.append(x)
            x -= 1
        args, seen = {}, set()
        for x in window:   # the delay slot first, then backwards: the first writer met is the last one executed
            d = dest_of(ws[x])
            if d in (4, 5, 6, 7) and d not in seen:
                seen.add(d)
                if refs[x] is not None:
                    args[d] = refs[x]
        out[t].append((lo + 4 * k, args))
    return out


class CodeMap:
    """the two .text sections aligned: other code address -> PAL code address, and the votes of the data references"""

    GAP_MAX = 64   # the instructions of a gap whose address-forming instructions are paired in order

    def __init__(self, pal, other, pal_functions=()):
        self.pal, self.other = pal, other
        plo, phi = pal.code
        olo, ohi = other.code
        self.pw = list(struct.unpack_from("<%dI" % ((phi - plo) // 4), pal.read(plo, phi - plo))) if phi > plo else []
        self.ow = list(struct.unpack_from("<%dI" % ((ohi - olo) // 4), other.read(olo, ohi - olo))) if ohi > olo else []
        self.functions = sorted(set(pal_functions))
        self.function_set = set(self.functions)
        self.o2p, self.p2o = {}, {}   # instruction index <-> instruction index
        self.equal_pairs = self.replace_pairs = 0
        prefs, ptoks = code_scan(pal, self.pw)
        orefs, otoks = code_scan(other, self.ow)
        self.ptoks = ptoks
        blocks = matching_blocks(ptoks, otoks)
        self.votes = collections.defaultdict(collections.Counter)       # PAL data address -> other address -> votes
        self.gap_votes = collections.defaultdict(collections.Counter)   # the same from the gaps (an object moved in or out of .sdata)
        i = j = 0
        for ma, mb, n in blocks:
            gi, gj = ma - i, mb - j
            if gi > 0 and gj > 0:
                if gi == gj:   # the same place, other instructions
                    for k in range(gi):
                        self.o2p[j + k], self.p2o[i + k] = i + k, j + k
                    self.replace_pairs += gi
                if gi <= self.GAP_MAX and gj <= self.GAP_MAX:
                    pr = [prefs[x] for x in range(i, ma) if prefs[x] is not None]
                    orr = [orefs[x] for x in range(j, mb) if orefs[x] is not None]
                    if pr and len(pr) == len(orr):
                        for p, o in zip(pr, orr):
                            self.gap_votes[p][o] += 1
            for k in range(n):
                self.o2p[mb + k], self.p2o[ma + k] = ma + k, mb + k
                p, o = prefs[ma + k], orefs[mb + k]
                if p is not None and o is not None:
                    self.votes[p][o] += 1
            self.equal_pairs += n
            i, j = ma + n, mb + n
        self.pal_refs = sorted({v for v in prefs if v is not None})
        self.other_refs = sorted({v for v in orefs if v is not None})
        # the arguments of the calls to one function, paired call site by call site when both have as many (InitLanguages
        # is called with the language table from another function on the other release)
        self.call_votes = collections.defaultdict(collections.Counter)
        pcalls, ocalls = call_sites(pal, self.pw, prefs), call_sites(other, self.ow, orefs)
        otargets = sorted(ocalls)
        for t in sorted(pcalls):
            j = self.p2o.get((t - plo) // 4) if plo <= t < phi else None
            if j is None:
                continue
            guess = olo + 4 * j
            i = bisect.bisect_left(otargets, guess - 16)
            near = [x for x in otargets[i:i + 8] if abs(x - guess) <= 16]
            if not near:
                continue
            ot = min(near, key=lambda x: (abs(x - guess), x))
            ps_, os_ = pcalls[t], ocalls[ot]
            if len(ps_) != len(os_) or len(ps_) > 16:
                continue
            for (_, pa), (_, oa) in zip(ps_, os_):
                for reg in sorted(pa):
                    if reg in oa:
                        self.call_votes[pa[reg]][oa[reg]] += 1

    def translate(self, o):
        """other code address -> PAL code address (inside any aligned instruction: the same byte of the PAL one), else None"""
        lo, hi = self.other.code
        if not (lo <= o < hi):
            return None
        i = self.o2p.get((o - lo) // 4)
        return None if i is None else self.pal.code[0] + 4 * i + (o - lo) % 4

    def function(self, o):
        """translate(o), moved to the start of the PAL function it lies in when that function's first instruction is aligned
        with one of the other's first instructions after o (the compiler scheduled a function's first instructions
        another way: `lui; lw; addiu sp` against `addiu sp; lui; lw`)"""
        t = self.translate(o)
        if t is None or t in self.function_set or not self.functions:
            return t
        k = bisect.bisect_right(self.functions, t) - 1
        if k < 0:
            return t
        f = self.functions[k]
        if t - f > 16:
            return t
        j = self.p2o.get((f - self.pal.code[0]) // 4)
        if j is None:
            return t
        of = self.other.code[0] + 4 * j
        return f if o <= of <= o + 16 else t

    def twins(self, f, g):
        """two PAL functions (starts) whose instructions are the same tokens"""
        if f not in self.function_set or g not in self.function_set:
            return False
        lo = self.pal.code[0]
        fe, ge = next_function(self.functions, f), next_function(self.functions, g)
        if fe - f != ge - g or fe - f > 4096:
            return False
        a, b = (f - lo) // 4, (g - lo) // 4
        n = (fe - f) // 4
        return self.ptoks[a:a + n] == self.ptoks[b:b + n]

    def vote(self, p, kinds=("code", "call", "code gap")):
        """the other address most PAL code references to p pair with (ties: the lowest), its votes and where from"""
        for votes, kind in ((self.votes, "code"), (self.call_votes, "call"), (self.gap_votes, "code gap")):
            c = votes.get(p)
            if c and kind in kinds:
                best = max(c.values())
                return min(o for o, n in c.items() if n == best), best, kind
        return None, 0, ""


# ------------------------------------------------------------------------------------------------------------- 2 data
def data_tokens(elf, addr, size, code_tr):
    out = []
    for w in words(elf.read(addr, size)):
        if w >= (1 << 32):
            out.append(w)
        elif elf.in_code(w):
            t = code_tr(w)
            out.append(CODE_TAG | t if t is not None else OTHER_CODE_TAG | w)
        elif elf.pointer_like(w) or w == elf.hi:
            s = elf.cstring(w) if w < elf.hi else None
            out.append(STRING_TAG | fnv40(s) if s is not None else POINTER_TOKEN)
        else:
            out.append(w)
    return out


def merge_ranges(rows):
    """contiguous rows of one displacement (and section) merged: [{section, pal, other, bytes, kind}]"""
    out = []
    for r in rows:
        if out and out[-1]["section"] == r["section"] and out[-1]["pal"] + out[-1]["bytes"] == r["pal"] and \
                out[-1]["other"] + out[-1]["bytes"] == r["other"]:
            out[-1]["bytes"] += r["bytes"]
            if out[-1]["kind"] != r["kind"]:
                out[-1]["kind"] = "mixed"
        else:
            out.append(dict(r))
    return out


def inside_one(starts, lo, hi):
    """no object of `starts` begins in (lo, hi)"""
    k = bisect.bisect_right(starts, lo)
    return k >= len(starts) or starts[k] >= hi


def align_blocks(pal, other, pa, pn, oa, on, code_tr, section, pal_starts=None, other_starts=None):
    """[(pal, other, bytes, kind)] of PAL [pa, pa+pn) against other [oa, oa+on), by content; the start rule inside one
    object on each side (no starts given: always)"""
    pt = data_tokens(pal, pa, pn, lambda w: w)
    ot = data_tokens(other, oa, on, code_tr)
    if pal_starts is None:
        rule = lambda i, ie, j, je: True   # noqa: E731
    else:
        rule = lambda i, ie, j, je: inside_one(pal_starts, pa + 4 * i, pa + 4 * ie) and inside_one(other_starts, oa + 4 * j, oa + 4 * je)   # noqa: E731
    rows = []
    for i, j, n, kind in aligned_runs(pt, ot, rule):
        b = min(4 * n, pa + pn - (pa + 4 * i), oa + on - (oa + 4 * j))
        if b > 0:
            rows.append({"section": section, "pal": pa + 4 * i, "other": oa + 4 * j, "bytes": b, "kind": kind})
    return rows


class Ranges:
    """address ranges pal <-> other: translate either way (sorted lists; the C++ port: upper_bound)"""

    def __init__(self, rows):
        self.by_other = sorted(((r["other"], r["pal"], r["bytes"]) for r in rows), key=lambda t: (t[0], t[1]))
        self.by_pal = sorted(((r["pal"], r["other"], r["bytes"]) for r in rows), key=lambda t: (t[0], t[1]))
        self.ok, self.pk = [t[0] for t in self.by_other], [t[0] for t in self.by_pal]

    @staticmethod
    def _find(keys, rows, v):
        i = bisect.bisect_right(keys, v) - 1
        if i >= 0 and rows[i][0] <= v < rows[i][0] + rows[i][2]:
            return rows[i][1] + (v - rows[i][0])
        return None

    def to_pal(self, v):
        return self._find(self.ok, self.by_other, v)

    def to_other(self, a):
        return self._find(self.pk, self.by_pal, a)

    def covers_pal(self, a):
        return self.to_other(a) is not None


# ------------------------------------------------------------------------------------------------------------- map
POINTER_SECTIONS = [".data", ".rodata", ".sdata"]   # the sections whose words are pointers (.vutext/.vudata: VU microcode)


def object_starts(elf, refs, symbols_pal, lo, hi):
    """the data objects of one executable in [lo, hi): the section starts, the addresses its code forms, the targets of
    its data pointers (a word of .data/.rodata/.sdata pointing at a word boundary or at the start of a string), and for
    PAL the data symbols"""
    starts = set(symbols_pal)
    for n in DATA_SECTIONS + BSS_SECTIONS:
        s = elf.sections.get(n)
        if s:
            starts.add(s["addr"])
    starts.update(v for v in refs if lo <= v < hi)
    for n in POINTER_SECTIONS:
        s = elf.sections.get(n)
        if not s:
            continue
        for w in words(elf.read(s["addr"], s["size"])):
            if w < (1 << 32) and not elf.in_code(w) and lo <= w < hi and w not in starts:
                if w % 4 == 0 or (elf.cstring(w) is not None and elf.read(w - 1, 1) == b"\0"):
                    starts.add(w)
    return sorted(a for a in starts if lo <= a < hi)


def extent(starts, a, section_end):
    i = bisect.bisect_right(starts, a)
    nxt = starts[i] if i < len(starts) else section_end
    return min(nxt, section_end) - a


def section_bounds(elf, a, names):
    for n in names:
        s = elf.sections.get(n)
        if s and s["addr"] <= a < s["addr"] + s["size"]:
            return n, s["addr"], s["addr"] + s["size"]
    return None, None, None


def read_engine_map(path):
    rows = {}
    if not path:
        return rows
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or line.startswith("pal_addr"):
            continue
        c = line.rstrip("\n").split("\t")
        if len(c) >= 4 and c[2].startswith("0x"):
            rows[int(c[0], 16)] = (int(c[2], 16), c[3])
    return rows


def read_data_names(path):
    rows = {}
    if not path:
        return rows
    for line in open(path, encoding="utf-8", errors="replace"):
        if line.startswith("#") or line.startswith("address"):
            continue
        c = line.rstrip("\n").split("\t")
        if len(c) >= 2 and c[0].startswith("0x"):
            rows[int(c[0], 16)] = c[1]
    return rows


def next_function(functions, p):
    i = bisect.bisect_right(functions, p)
    return functions[i] if i < len(functions) else 0xFFFFFFFF


def row_words(r):
    """(PAL word address, other address) of every whole PAL word a row covers (a row may start at any byte)"""
    w = (r["pal"] + 3) & ~3
    while w + 4 <= r["pal"] + r["bytes"]:
        yield w, r["other"] + (w - r["pal"])
        w += 4


def cut_ranges(rows, cuts):
    """rows (PAL-sorted, disjoint) minus the PAL ranges `cuts` [(lo, hi)]"""
    out = []
    for r in rows:
        pieces = [(r["pal"], r["pal"] + r["bytes"])]
        for lo, hi in cuts:
            nxt = []
            for a, b in pieces:
                if hi <= a or lo >= b:
                    nxt.append((a, b))
                    continue
                if a < lo:
                    nxt.append((a, lo))
                if hi < b:
                    nxt.append((hi, b))
            pieces = nxt
        for a, b in pieces:
            out.append({"section": r["section"], "pal": a, "other": r["other"] + (a - r["pal"]), "bytes": b - a, "kind": r["kind"]})
    return out


def make_map(pal, other, symbols, retail_image=None, engine_map=None, serial="", exe="", data_names=None, codemap=None, census=None):
    """The map (a dict: the JSON written) of `other` against `pal`. `census` (a dict, optional) receives the tables behind the
    map's counts: every PAL data symbol's counterpart with its evidence and sizes, every PAL object, and -- with
    `retail_image` / `engine_map` / `data_names` -- the symbols the game's objects reference, the engine's function map
    compared and the other executable's known data words named. The map itself depends on the two executables and
    `symbols` only (tools/discs/elf_region_map.py and engine/app/host/region_map_make.cpp make the same file)."""
    hx = lambda v: "0x%08X" % v   # noqa: E731
    pal_functions = sorted({a for a, f in symbols.values() if f})
    # ---- 1 code
    cm = codemap or CodeMap(pal, other, pal_functions)
    code_tr = cm.function
    # ---- the objects of both executables (they bound the start rule and the extents)
    pal_data_syms = sorted({a for a, f in symbols.values() if not f and PAL_IMAGE_START <= a < PAL_IMAGE_END})
    pal_starts = object_starts(pal, cm.pal_refs, pal_data_syms, PAL_IMAGE_START, PAL_IMAGE_END)
    other_starts0 = object_starts(other, cm.other_refs, [], other.image_lo, other.hi)
    # ---- 2 data: each section by content (evidence)
    content, sections = [], []
    for name in DATA_SECTIONS:
        ps, os_ = pal.sections.get(name), other.sections.get(name)
        if not ps or not os_ or ps["type"] == SHT_NOBITS or os_["type"] == SHT_NOBITS:
            sections.append({"section": name, "pal_bytes": ps["size"] if ps else 0, "other_bytes": os_["size"] if os_ else 0,
                             "aligned_bytes": 0, "why": "not in both executables"})
            continue
        rows = align_blocks(pal, other, ps["addr"], ps["size"], os_["addr"], os_["size"], code_tr, name, pal_starts, other_starts0)
        content += rows
        sections.append({"section": name, "pal": hx(ps["addr"]), "pal_bytes": ps["size"], "other": hx(os_["addr"]),
                         "other_bytes": os_["size"], "aligned_bytes": sum(r["bytes"] for r in rows)})
    content.sort(key=lambda r: r["pal"])
    cr = Ranges(content)
    content_starts = [r["pal"] for r in content]

    def content_run(a):
        k = bisect.bisect_right(content_starts, a) - 1
        return content[k] if k >= 0 and content[k]["pal"] <= a < content[k]["pal"] + content[k]["bytes"] else None
    # the data pointers the aligned words hold on both sides: this PAL object is that one (the same field's target) --
    # when the two targets hold the same thing: the same string, or (word-aligned) the same first words
    ptr_votes = collections.defaultdict(collections.Counter)
    for r in content:
        for w, v in row_words(r):
            pw, ow = pal.word(w), other.word(v)
            if not (pal.pointer_like(pw) and not pal.in_code(pw) and other.pointer_like(ow) and not other.in_code(ow)):
                continue
            ps_, os2 = pal.cstring(pw), other.cstring(ow)
            if ps_ is not None or os2 is not None:
                same = ps_ == os2
            else:
                same = pw % 4 == 0 and ow % 4 == 0 and data_tokens(pal, pw, 16, lambda x: x) == data_tokens(other, ow, 16, code_tr)
            if same:
                ptr_votes[pw][ow] += 1
    # ---- 3 every PAL data object's counterpart: the strongest evidence, one other object per PAL object
    STRENGTH = {"content": 7, "code": 6, "call": 5, "pointers": 4, "content (part)": 3, "code gap": 2, "elsewhere": 1, "neighbours": 0}

    def best_of(c):
        if not c:
            return None
        m = max(c.values())
        return min(o for o, n in c.items() if n == m)
    objs = []   # PAL objects in the data sections: (a, n, section)
    for a in pal_starts:
        sec, slo, shi = section_bounds(pal, a, DATA_SECTIONS)
        if sec is not None:
            objs.append((a, extent(pal_starts, a, shi), sec))
    objects, decided = {}, []
    for a, n, sec in objs:
        cands = []
        o_c = cr.to_other(a)
        if o_c is not None:
            run = content_run(a)
            strong = run["kind"] == "equal" and run["pal"] + run["bytes"] >= a + n
            cands.append(("content" if strong else "content (part)", o_c))
        for kind in ("code", "call", "code gap"):
            o, _, _ = cm.vote(a, (kind,))
            if o is not None:
                cands.append((kind, o))
        o_p = best_of(ptr_votes.get(a))
        if o_p is not None:
            cands.append(("pointers", o_p))
        cands.sort(key=lambda t: -STRENGTH[t[0]])
        objects[a] = {"section": sec, "bytes": n, "candidates": cands, "other": None, "evidence": "none"}
        if cands:
            decided.append((STRENGTH[cands[0][0]], a, cands[0][1], cands[0][0]))
    # claims: the stronger evidence keeps an other address range; a weaker claim on it loses
    claimed = []   # sorted (lo, hi, PAL object)

    def claim(o, on_, a):
        k = bisect.bisect_right(claimed, (o, 0xFFFFFFFF, 0))
        for x in claimed[max(0, k - 1):k + 1]:
            if x[0] < o + on_ and o < x[1]:
                return x[2]
        bisect.insort(claimed, (o, o + on_, a))
        return None
    decided.sort(key=lambda t: (-t[0], t[1]))
    for strength, a, o, kind in decided:
        osec, oslo, oshi = section_bounds(other, o, DATA_SECTIONS)
        if osec is None:
            objects[a]["evidence"] = "%s (into %s: not loaded)" % (kind, hx(o))
            continue
        on_ = max(1, min(objects[a]["bytes"], extent(other_starts0, o, oshi)))
        winner = claim(o, on_, a)
        if winner is not None:
            objects[a]["evidence"] = "%s %s lost to %s" % (kind, hx(o), hx(winner))
            continue
        objects[a]["other"], objects[a]["evidence"] = o, kind
    # the same tokens elsewhere (an object moved by code that changed), then the neighbours' displacement
    data_ranges_other, other_tokens = [], {}
    for name in POINTER_SECTIONS:
        s_ = other.sections.get(name)
        if s_:
            data_ranges_other.append((s_["addr"], s_["addr"] + s_["size"]))
            other_tokens[s_["addr"]] = data_tokens(other, s_["addr"], s_["size"], code_tr)
    for a, n, sec in objs:
        ob = objects[a]
        if ob["other"] is not None or n < 8 or a % 4:
            continue
        pt = data_tokens(pal, a, min(n, 64), lambda w: w)
        if all(t == 0 or t == POINTER_TOKEN for t in pt[:2]):
            continue
        # the longest prefix (two words at least) found at exactly one place of the other's data sections
        best_len, hits = 0, []
        for lo, hi in data_ranges_other:
            ot = other_tokens[lo]
            first = pt[0]
            for k in range(len(ot)):
                if ot[k] != first:
                    continue
                m = 0
                while m < len(pt) and k + m < len(ot) and ot[k + m] == pt[m]:
                    m += 1
                if m < 2:
                    continue
                if m > best_len:
                    best_len, hits = m, [lo + 4 * k]
                elif m == best_len:
                    hits.append(lo + 4 * k)
        if len(hits) == 1 and claim(hits[0], 4 * best_len, a) is None:
            ob["other"], ob["evidence"] = hits[0], "elsewhere"
    for k, (a, n, sec) in enumerate(objs):
        ob = objects[a]
        if ob["other"] is not None or k == 0 or k + 1 >= len(objs):
            continue
        pa, pn_ = objs[k - 1][0], objs[k + 1][0]
        po, no = objects[pa]["other"], objects[pn_]["other"]
        if po is None or no is None or po - pa != no - pn_ or objs[k - 1][2] != sec or objs[k + 1][2] != sec:
            continue
        o = a + (po - pa)
        if section_bounds(other, o, DATA_SECTIONS)[0] is not None and claim(o, n, a) is None:
            ob["other"], ob["evidence"] = o, "neighbours"
    # the other executable's objects: its own starts and its counterparts of the PAL objects (an object's counterpart
    # extends to the next counterpart: the other's own starts inside it are fields of it, or only the other's)
    other_starts = sorted(set(other_starts0) | {ob["other"] for ob in objects.values() if ob["other"] is not None})
    counterpart_starts = sorted({ob["other"] for ob in objects.values() if ob["other"] is not None})
    # ---- the layout, object by object: an object whole in one equal run as the run says; any other aligned on its own
    rows_out, moved = [], 0
    for a, n, sec in objs:
        ob = objects[a]
        o = ob["other"]
        if o is None:
            continue
        osec, oslo, oshi = section_bounds(other, o, DATA_SECTIONS)
        on_ = extent(counterpart_starts, o, oshi)
        ob["other_bytes"] = on_
        if ob["evidence"] == "content":
            rows_out.append({"section": sec, "pal": a, "other": o, "bytes": n, "kind": "content equal"})
            continue
        rows = align_blocks(pal, other, a, n, o, on_, code_tr, sec, [a], [o])
        if not rows:   # nothing alike: the first bytes, the same place
            rows = [{"section": sec, "pal": a, "other": o, "bytes": min(n, on_), "kind": "start"}]
        if ob["evidence"] != "content (part)":
            moved += 1
        for r in rows:
            r["kind"] = ob["evidence"] + " " + r["kind"]
        rows_out += rows
    moved_rows = [r for r in rows_out if not r["kind"].startswith("content")]
    segments = merge_ranges(sorted(rows_out, key=lambda r: r["pal"]))
    # the file names the two programs spell differently: a matched string object naming a path, other on each side
    # (the PAL C++ names some files by its own literals: the loader gives the disc's archive entries the PAL names)
    file_names = []
    for a, n, sec in objs:
        o = objects[a]["other"]
        if o is None:
            continue
        ps_, os2 = pal.cstring(a), other.cstring(o)
        if ps_ is not None and os2 is not None and ps_ != os2 and b"\\" in ps_ and b"\\" in os2:
            file_names.append({"pal": ps_.decode("latin1"), "other": os2.decode("latin1"), "at": hx(a)})
    # ---- 4 bss: the section starts, checked against the code's votes
    bss, bss_check = [], collections.Counter()
    for name in BSS_SECTIONS:
        ps, os_ = pal.sections.get(name), other.sections.get(name)
        if not ps or not os_:
            continue
        d = os_["addr"] - ps["addr"]
        n = min(ps["size"], os_["size"])
        bss.append({"section": name, "pal": ps["addr"], "other": ps["addr"] + d, "bytes": n, "kind": "section start"})
        for p in sorted(cm.votes):
            if ps["addr"] <= p < ps["addr"] + ps["size"]:
                o, _, _ = cm.vote(p)
                bss_check["agree" if o - p == d else "disagree"] += 1
    # ---- 5 pointers, over the laid-out image (segments, then the placements they lead to)
    placed = []
    def covered(p):
        return Ranges(segments).covers_pal(p) or any(r["pal"] <= p < r["pal"] + r["bytes"] for r in placed)

    def pal_only_run(p):
        """the run of PAL data bytes around p that no segment covers (within p's section)"""
        sec, slo, shi = section_bounds(pal, p, DATA_SECTIONS)
        if sec is None or covered(p):
            return None
        lo, hi = p, p
        while lo > slo and not covered(lo - 1):
            lo -= 1
        while hi < shi and not covered(hi):
            hi += 1
        return lo, hi

    ranges = Ranges(segments + bss)
    pointers, code_pairs, data_exc_votes = [], {}, collections.defaultdict(collections.Counter)
    stats = collections.Counter()
    no_pal_function, unmapped_data, other_function = [], [], []
    fills = {}
    todo = [dict(r) for r in segments]
    while todo:
        r = todo.pop(0)
        for w, v in row_words(r):
            ow, pw = other.word(v), pal.word(w)
            t = None
            if other.in_code(ow):   # every code word of the laid-out image has its pair (the code map), translated or not
                t = code_tr(ow)
                if t is not None:
                    code_pairs[ow] = t
            if ow == pw or not (other.pointer_like(ow) or ow == other.hi):
                continue
            if other.in_code(ow):
                if t is not None and t != pw and pw in cm.function_set and (pw <= t < next_function(cm.functions, pw) or cm.twins(pw, t)):
                    # inside the PAL slot's own function, or a function with the same code as it (the alignment picked
                    # the other of two identical bodies): the slot's own function, at this word only
                    fills[w] = pw
                    stats["code_same_function"] += 1
                elif t is not None:
                    pointers.append(w)
                    stats["code"] += 1
                    if pal.in_code(pw) and t != pw:
                        stats["code_other_function"] += 1
                        other_function.append([hx(w), hx(ow), hx(t), hx(pw)])
                elif pal.in_code(pw):
                    fills[w] = pw
                    stats["code_no_pal_function_pal_slot"] += 1
                    no_pal_function.append([hx(w), hx(ow), hx(pw)])
                else:
                    stats["code_no_pal_function"] += 1
                    no_pal_function.append([hx(w), hx(ow), ""])
                continue
            # a data pointer (or the end of the image)
            t = pal.hi if ow == other.hi else ranges.to_pal(ow)
            if t is not None:
                pointers.append(w)
                stats["data"] += 1
                if r["kind"].endswith(("equal", "mixed")) and pal.pointer_like(pw) and t != pw:
                    data_exc_votes[ow][pw] += 1
                continue
            # an object only the other executable has: into the PAL-only object the PAL word pointed to, when it fits
            osec, oslo, oshi = section_bounds(other, ow, DATA_SECTIONS)
            if osec is not None and pal.pointer_like(pw) and not pal.in_code(pw):
                run = pal_only_run(pw)
                on_ = extent(other_starts, ow, oshi)
                if run and pw + on_ <= run[1]:
                    psec, _, _ = section_bounds(pal, pw, DATA_SECTIONS)
                    row = {"section": psec, "pal": pw, "other": ow, "bytes": on_, "kind": "placed"}
                    placed.append(row)
                    todo.append(dict(row))
                    ranges = Ranges(segments + placed + bss)
                    pointers.append(w)
                    stats["data_placed"] += 1
                    continue
            stats["data_unmapped"] += 1
            unmapped_data.append([hx(w), hx(ow), hx(pw) if pal.pointer_like(pw) else ""])
    data_exceptions = {o: min(p for p, n in c.items() if n == max(c.values())) for o, c in data_exc_votes.items()}
    if other.hi != pal.hi:
        data_exceptions[other.hi] = pal.hi
    segments = merge_ranges(sorted(segments + placed, key=lambda r: r["pal"]))
    # ---- 6 fills: a PAL object no byte of the other executable reaches keeps its PAL code pointers
    final = Ranges(segments)
    pal_only_objects = []
    for a in pal_starts:
        sec, slo, shi = section_bounds(pal, a, DATA_SECTIONS)
        if sec is None:
            continue
        n = extent(pal_starts, a, shi)
        if any(final.covers_pal(x) for x in range(a, a + n)):
            continue
        pal_only_objects.append((a, n))
        for w in range((a + 3) & ~3, a + n - 3, 4):
            if not (PATCH_START <= w < PATCH_END):
                continue
            pw = pal.word(w)
            if pal.in_code(pw):
                fills[w] = pw
                stats["fill_pal_only_object"] += 1
    # the other executable's addresses two PAL places come from (a pointer to one of them translates to the first)
    srt = sorted((r["other"], r["other"] + r["bytes"], r["pal"]) for r in segments)
    other_overlaps = sum(1 for x, y in zip(srt, srt[1:]) if y[0] < x[1])
    # ---- the census: every PAL data symbol
    names = {}
    for n, (a, f) in symbols.items():
        if not f and PAL_IMAGE_START <= a < PAL_IMAGE_END:
            names.setdefault(a, []).append(n)
    decomp = dict(retail_image.get("symbols", {})) if retail_image else {}
    all_names = dict(names)
    for n, a in decomp.items():
        if n not in symbols:
            all_names.setdefault(a, []).append(n)
    sym_rows, ev_count, size_count = {}, collections.Counter(), collections.Counter()
    bss_r = Ranges(bss)
    for a in sorted(all_names):
        sec, slo, shi = section_bounds(pal, a, DATA_SECTIONS)
        if sec is None:
            bsec, blo, bhi = section_bounds(pal, a, BSS_SECTIONS)
            o = bss_r.to_other(a) if bsec else None
            o_v, _, _ = cm.vote(a)
            ev = ("bss" + ("+code" if o_v == o else (" (the code says %s)" % hx(o_v) if o_v is not None else ""))) if o is not None else "none"
            row = {"pal": hx(a), "other": hx(o) if o is not None else None, "section": bsec or "gap", "evidence": ev, "size": "bss"}
        else:
            ob = objects.get(a, {"evidence": "none", "other": None})
            pn = extent(pal_starts, a, shi)
            cov = sum(1 for x in range(a, a + pn) if final.covers_pal(x))
            o = final.to_other(a)
            on_ = 0
            if o is not None:
                osec, oslo, oshi = section_bounds(other, o, DATA_SECTIONS)
                on_ = extent(other_starts, o, oshi) if osec else 0
            ev = ob["evidence"]
            if a not in objects and o is not None:
                ev = "inside"   # not an object start of the PAL program (a name only the game's objects use): inside another's layout
            elif ob["other"] is None and o is not None:
                ev = "placed"   # an object only the other executable has, put into this PAL-only object (the same field's target)
            if o is None:
                size = "unmatched"
            elif cov < pn:
                size = "other smaller"
            elif on_ > pn:
                size = "other larger"
            else:
                size = "same"
            row = {"pal": hx(a), "pal_bytes": pn, "other": hx(o) if o is not None else None, "other_bytes": on_, "covered": cov,
                   "section": sec, "evidence": ev, "size": size}
        if data_names and o is not None and o in data_names:
            row["named"] = data_names[o]
        for n in all_names[a]:
            sym_rows[n] = dict(row, decomp=n in decomp, symbol_addrs=n in symbols)
    in_symbol_addrs = {n: r for n, r in sym_rows.items() if r["symbol_addrs"]}
    for r in in_symbol_addrs.values():
        ev_count[r["evidence"].split(" ")[0]] += 1
        size_count[r["size"]] += 1
    # ---- the composed image against the PAL one (the differences: addresses and sizes only)
    composed, _ = apply_map({"image": {"pal_start": hx(PAL_IMAGE_START), "pal_bss_start": hx(PAL_BSS_START), "pal_end": hx(PAL_IMAGE_END)},
                             "segments": [{"pal": r["pal"], "other": r["other"], "bytes": r["bytes"]} for r in segments],
                             "bss": [{"pal": r["pal"], "other": r["other"], "bytes": r["bytes"]} for r in bss],
                             "pointers": pointers, "code": [[o, p] for o, p in code_pairs.items()],
                             "data": [[o, p] for o, p in data_exceptions.items()], "fills": [[w, v] for w, v in fills.items()]}, other)
    native = native_image(pal)
    diffs, i, N = [], 0, PAL_BSS_START - PAL_IMAGE_START
    while i < N:
        if composed[i] != native[i]:
            j = i
            while j < N and (composed[j] != native[j] or (j + 1 < N and j + 1 - i < 4 and composed[j + 1] != native[j + 1])):
                j += 1
            diffs.append((PAL_IMAGE_START + i, j - i))
            i = j
        else:
            i += 1
    def name_of(a):
        """the PAL object a byte lies in: its symbol_addrs.txt name, else D_<address> (the decompilation's form), + the offset"""
        k = bisect.bisect_right(pal_starts, a) - 1
        if k < 0:
            return hx(a)
        o = pal_starts[k]
        return "%s+0x%X" % (names[o][0] if o in names else "D_%08X" % o, a - o)
    diff_rows = []
    for a, n in diffs:
        cov = sum(1 for x in range(a, a + n) if final.covers_pal(x))
        diff_rows.append({"pal": hx(a), "bytes": n, "at": name_of(a),
                          "kind": "the other executable's bytes" if cov == n else ("zero (no counterpart)" if cov == 0 else "mixed")})
    engine = None
    if engine_map and census is not None:
        agree = disagree = absent = 0
        for p in pal_functions:
            r_ = engine_map.get(p)
            if not r_:
                absent += 1
            elif code_tr(r_[0]) == p:
                agree += 1
            else:
                disagree += 1
        engine = {"agree": agree, "disagree": disagree, "not_in_engine_map": absent}
    fset = set(pal_functions)
    coverage = {
        "data_image_bytes": PAL_BSS_START - PAL_IMAGE_START,
        "aligned_bytes": sum(r["bytes"] for r in segments),
        "content_bytes": sum(r["bytes"] for r in rows_out if r["kind"].startswith("content")),
        "moved_bytes": sum(r["bytes"] for r in moved_rows),
        "placed_bytes": sum(r["bytes"] for r in placed),
        "pointer_words": len(pointers),
        "code_pairs": len(code_pairs),
        "code_pairs_at_function_starts": sum(1 for p in code_pairs.values() if p in fset),
        "code_conflicts": 0,
        "code_instructions": [len(cm.pw), len(cm.ow)],
        "code_aligned": {"equal": cm.equal_pairs, "replace": cm.replace_pairs},
        "code_pointer_words": stats["code"],
        "code_pointers_to_another_function": stats["code_other_function"],
        "code_pointers_snapped_to_the_slot_function": stats["code_same_function"],
        "code_pointers_no_pal_function": stats["code_no_pal_function"] + stats["code_no_pal_function_pal_slot"],
        "code_pointers_no_pal_function_pal_slot": stats["code_no_pal_function_pal_slot"],
        "data_pointer_words": stats["data"] + stats["data_placed"],
        "data_pointers_placed": stats["data_placed"],
        "data_pointers_unmapped": stats["data_unmapped"],
        "data_exceptions": len(data_exceptions),
        "fills": len(fills),
        "fills_pal_only_objects": stats["fill_pal_only_object"],
        "names": len(file_names),
        "objects": len(objects),
        "objects_moved": moved,
        "objects_by_evidence": dict(sorted(collections.Counter(ob["evidence"].split(" ")[0] if ob["other"] is not None else "none" for ob in objects.values()).items())),
        "objects_pal_only": len(pal_only_objects),
        "other_overlaps": other_overlaps,
        "bss_votes": dict(sorted(bss_check.items())),
        "pal_data_symbols": len(in_symbol_addrs),
        "pal_data_symbols_mapped": sum(1 for r in in_symbol_addrs.values() if r["other"] is not None),
        "symbols_by_evidence": dict(sorted(ev_count.items())),
        "symbols_by_size": dict(sorted(size_count.items())),
        "differing_bytes": sum(n for _, n in diffs),
    }
    if census is not None:
        census["symbols"] = {n: sym_rows[n] for n in sorted(sym_rows)}
        census["objects"] = [[hx(a), objects[a]["bytes"], hx(objects[a]["other"]) if objects[a]["other"] is not None else None,
                              objects[a].get("other_bytes", 0), objects[a]["evidence"]] for a, _, _ in objs]
        census["engine_function_map"] = engine
        if decomp:
            census["decomp_referenced"] = len(decomp)
            census["decomp_referenced_mapped"] = sum(1 for n in decomp if n in sym_rows and sym_rows[n]["other"] is not None)
            census["decomp_referenced_by_evidence"] = dict(sorted(collections.Counter(
                sym_rows[n]["evidence"].split(" ")[0] for n in decomp if n in sym_rows).items()))
            census["decomp_referenced_by_size"] = dict(sorted(collections.Counter(
                sym_rows[n]["size"] for n in decomp if n in sym_rows).items()))
    return {
        "schema": SCHEMA,
        "kind": "region-map",
        "serial": serial,
        "executable": exe,
        "elf": {"sha256": other.sha256(), "bytes": len(other.data), "entry": hx(other.entry), "load": [hx(other.lo), hx(other.hi)],
                "text": [hx(other.code[0]), hx(other.code[1])]},
        "pal_elf": {"sha256": pal.sha256(), "bytes": len(pal.data), "entry": hx(pal.entry)},
        "image": {"pal_start": hx(PAL_IMAGE_START), "pal_bss_start": hx(PAL_BSS_START), "pal_end": hx(PAL_IMAGE_END)},
        "made_by": MADE_BY,
        "sections": sections,
        "segments": [{"section": s["section"], "pal": hx(s["pal"]), "other": hx(s["other"]), "bytes": s["bytes"], "kind": s["kind"]} for s in segments],
        "bss": [{"section": s["section"], "pal": hx(s["pal"]), "other": hx(s["other"]), "bytes": s["bytes"], "kind": s["kind"]} for s in bss],
        "pointers": [hx(a) for a in sorted(set(pointers))],
        "code": [[hx(o), hx(p)] for o, p in sorted(code_pairs.items())],
        "data": [[hx(o), hx(p)] for o, p in sorted(data_exceptions.items())],
        "fills": [[hx(w), hx(v)] for w, v in sorted(fills.items())],
        "names": file_names,
        "coverage": coverage,
        "differences": diff_rows,
        "code_pointers_to_another_function": other_function,
        "code_without_pal_function": no_pal_function,
        "data_pointers_unmapped": unmapped_data,
    }

# ------------------------------------------------------------------------------------------------------------- apply
def u32(v):
    return int(v, 16) if isinstance(v, str) else int(v)


def translate(v, segs):
    """other address -> PAL address through [(pal, other, bytes)] sorted by other; None outside."""
    starts = [s[1] for s in segs]
    i = bisect.bisect_right(starts, v) - 1
    if i >= 0 and segs[i][1] <= v < segs[i][1] + segs[i][2]:
        return segs[i][0] + (v - segs[i][1])
    return None


def apply_map(doc, other):
    """The PAL data image [PAL_IMAGE_START, PAL_IMAGE_END) built from `other` through the map (as the runner's loader does):
    zero, the segments copied, the pointer words translated (code pairs, data pairs, the ranges), the fills written."""
    start, bss_start, end = u32(doc["image"]["pal_start"]), u32(doc["image"]["pal_bss_start"]), u32(doc["image"]["pal_end"])
    mem = bytearray(end - start)
    segs = [(u32(s["pal"]), u32(s["other"]), int(s["bytes"])) for s in doc["segments"]]
    for pa, oa, n in segs:
        lo, hi = max(pa, start), min(pa + n, bss_start)
        if lo < hi:
            mem[lo - start:hi - start] = other.read(oa + (lo - pa), hi - lo)
    by_other = sorted(segs + [(u32(s["pal"]), u32(s["other"]), int(s["bytes"])) for s in doc["bss"]], key=lambda t: (t[1], t[0]))
    code = {u32(o): u32(p) for o, p in doc["code"]}
    data = {u32(o): u32(p) for o, p in doc.get("data", [])}
    stats = collections.Counter()
    for a in sorted({u32(a) for a in doc["pointers"]}):
        if not (start <= a and a + 4 <= bss_start):
            continue
        v = struct.unpack_from("<I", mem, a - start)[0]
        kind = "code" if v in code else ("data" if v in data else "segment")
        t = code.get(v, data.get(v))
        if t is None:
            t = translate(v, by_other)
        if t is None:
            stats["unmapped"] += 1
            continue
        stats[kind] += 1
        struct.pack_into("<I", mem, a - start, t)
    for w, v in doc.get("fills", []):
        w = u32(w)
        if start <= w and w + 4 <= bss_start:
            struct.pack_into("<I", mem, w - start, u32(v))
            stats["fills"] += 1
    return bytes(mem), stats


def native_image(pal):
    """What runner/te_env.cpp LoadRetailImage leaves at [PAL_IMAGE_START, PAL_IMAGE_END): the file bytes, bss zero."""
    mem = bytearray(PAL_IMAGE_END - PAL_IMAGE_START)
    mem[:PAL_BSS_START - PAL_IMAGE_START] = pal.read(PAL_IMAGE_START, PAL_BSS_START - PAL_IMAGE_START)
    return bytes(mem)


# ------------------------------------------------------------------------------------------------------------- shift
def make_shifted(pal, shift):
    """A synthetic executable: every section and segment moved by `shift`, every pointer word of the allocated data
    sections rebased by `shift`, and -- when the shift is a multiple of 64 KB -- every lui of .text whose high half
    points into the program raised by shift >> 16 (the addresses the code forms move with the data, as a relink moves
    them: the code's votes are then right too; with another shift the code keeps its old addresses)."""
    d = bytearray(pal.data)
    struct.pack_into("<I", d, 24, pal.entry + shift)
    phoff, shoff = struct.unpack_from("<II", d, 28)
    phentsize, phnum, shentsize, shnum, _ = struct.unpack_from("<HHHHH", d, 42)
    for i in range(phnum):
        at = phoff + i * phentsize
        if struct.unpack_from("<I", d, at)[0] == 1:
            v, p = struct.unpack_from("<II", d, at + 8)
            struct.pack_into("<II", d, at + 8, v + shift, p + shift)
    rebased = ambiguous = rebased_lui = kept = 0
    for i in range(shnum):
        at = shoff + i * shentsize
        name_off, typ, flags, addr, off, size = struct.unpack_from("<6I", d, at)
        if not (flags & SHF_ALLOC) or not addr:
            continue
        struct.pack_into("<I", d, at + 12, addr + shift)
        if typ == SHT_NOBITS:
            continue
        if addr == pal.code[0]:
            if shift % 0x10000 == 0:
                for k in range(0, size - size % 4, 4):
                    w = struct.unpack_from("<I", d, off + k)[0]
                    if w >> 26 == OP_LUI and pal.lo <= (w & 0xFFFF) << 16 < pal.hi:
                        struct.pack_into("<I", d, off + k, (w & 0xFFFF0000) | (((w & 0xFFFF) + (shift >> 16)) & 0xFFFF))
                        rebased_lui += 1
            continue
        for k in range(0, size - size % 4, 4):
            w = struct.unpack_from("<I", d, off + k)[0]
            if pal.pointer_like(w) and not (w >> 24 == 0 and all(0x20 <= (w >> s_) & 0xFF < 0x7F for s_ in (0, 8, 16))):
                # (a word "xyz\0" is the end of a string as often as a pointer: left alone -- a pointer left alone still
                # rebuilds the PAL image, a string rebased would not be the same string)
                struct.pack_into("<I", d, off + k, w + shift)
                rebased += 1
            elif pal.pointer_like(w):
                kept += 1
            elif pal.hi <= w < pal.hi + shift:
                ambiguous += 1
    make_shifted.rebased_lui = rebased_lui
    make_shifted.kept = kept
    return bytes(d), rebased, ambiguous


# ------------------------------------------------------------------------------------------------------------- census
def census(pal, symbols, retail_image):
    secs = [(n, s["addr"], s["addr"] + s["size"]) for n, s in sorted(pal.sections.items(), key=lambda t: t[1]["addr"])]
    out = {"elf": {"sha256": pal.sha256(), "bytes": len(pal.data), "entry": "0x%08X" % pal.entry,
                   "loads": [["0x%08X" % v, "0x%X" % f, "0x%X" % m] for v, _, f, m in pal.loads]},
           "sections": [[n, "0x%08X" % lo, "0x%08X" % hi, hi - lo] for n, lo, hi in secs]}
    funcs = {a for a, f in symbols.values() if f}
    scan = pal.read(0x2E6F00, 0x30A460 - 0x2E6F00)
    ws = words(scan)
    code_words = [w for w in ws if pal.in_code(w) or 0x100000 <= w < 0x2D9D90]
    out["code_pointer_scan"] = {"range": ["0x002E6F00", "0x0030A460"], "words": len(ws), "in_code_range": len(code_words),
                                "at_function_starts": sum(1 for w in code_words if w in funcs)}
    cls = collections.Counter()
    for n, (a, f) in symbols.items():
        if f or not (PAL_IMAGE_START <= a < PAL_IMAGE_END):
            continue
        cls["%s %s" % (section_of(a, secs) or "gap", "D_" if re.fullmatch(r"D_[0-9A-F]{8}", n) else ("jtbl" if n.startswith("jtbl_") else "named"))] += 1
    out["symbol_addrs_in_image"] = dict(sorted(cls.items()))
    if retail_image:
        ref = collections.Counter(section_of(a, secs) or "gap" for a in retail_image["symbols"].values())
        out["decomp_references"] = {"symbols": len(retail_image["symbols"]), "by_section": dict(sorted(ref.items()))}
    return out


# ------------------------------------------------------------------------------------------------------------- selftest
def synthetic_elf(base_text, sections, entry_off=8):
    """A tiny ELF: one PT_LOAD from base_text, sections [(name, addr, bytes or ('nobits', n))]."""
    names = b"\0" + b"\0".join(n.encode() for n, _, _ in sections) + b"\0.shstrtab\0"
    lo = base_text
    file_end = max(a + len(b) for _, a, b in sections if not isinstance(b, tuple))
    mem_end = max(a + (b[1] if isinstance(b, tuple) else len(b)) for _, a, b in sections)
    body = bytearray(file_end - lo)
    for _, a, b in sections:
        if not isinstance(b, tuple):
            body[a - lo:a - lo + len(b)] = b
    ehsize, phentsize, shentsize = 52, 32, 40
    phoff, data_off = ehsize, 0x1000
    strtab_off = data_off + len(body)
    shoff = (strtab_off + len(names) + 3) & ~3
    shnum = len(sections) + 2
    out = bytearray(shoff + shnum * shentsize)
    out[:4] = b"\x7fELF"
    out[4], out[5], out[6] = 1, 1, 1
    struct.pack_into("<HHIIIIIHHHHHH", out, 16, 2, 8, 1, lo + entry_off, phoff, shoff, 0, ehsize, phentsize, 1, shentsize, shnum, shnum - 1)
    struct.pack_into("<8I", out, phoff, 1, data_off, lo, lo, len(body), mem_end - lo, 7, 0x1000)
    out[data_off:data_off + len(body)] = body
    out[strtab_off:strtab_off + len(names)] = names
    name_at = 1
    for i, (n, a, b) in enumerate(sections, 1):
        nobits = isinstance(b, tuple)
        size = b[1] if nobits else len(b)
        struct.pack_into("<10I", out, shoff + i * shentsize, name_at, SHT_NOBITS if nobits else 1, SHF_ALLOC | 1, a,
                         data_off + (a - lo) if not nobits else data_off + len(body), size, 0, 0, 4, 0)
        name_at += len(n) + 1
    struct.pack_into("<10I", out, shoff + (shnum - 1) * shentsize, name_at, 3, 0, 0, strtab_off, len(names), 0, 0, 1, 0)
    return bytes(out)


def selftest():
    import random
    rnd = random.Random(7)
    text_lo, n_text = 0x100000, 0x400
    funcs = [text_lo + 0x40 * i for i in range(n_text // 0x40)]
    text = bytearray()
    for i in range(n_text // 4):   # distinct instructions (addiu with other constants), so the code aligns
        text += struct.pack("<I", 0x24020000 | (i * 37 & 0x7FFF) if i % 16 else 0x03E00008)
    data_lo = text_lo + n_text
    data = bytearray()
    for i in range(200):   # floats, small integers, code pointers, data pointers, strings
        r = i % 5
        if r == 0:
            data += struct.pack("<I", funcs[i % len(funcs)])
        elif r == 1:
            data += struct.pack("<I", data_lo + 4 * ((i * 7) % 200))
        elif r == 2:
            data += struct.pack("<f", rnd.random())
        elif r == 3:
            data += struct.pack("<I", rnd.randrange(0, 1000))
        else:
            data += b"abcd"
    bss_lo = data_lo + len(data)
    data[8:12] = struct.pack("<I", bss_lo + 16)   # a pointer into .bss
    secs = [(".text", text_lo, bytes(text)), (".data", data_lo, bytes(data)), (".bss", bss_lo, ("nobits", 0x100))]
    pal = Elf(synthetic_elf(text_lo, secs), "pal")
    symbols = {"FUN_%08X" % a: (a, True) for a in funcs}
    symbols["D_%08X" % (data_lo + 40)] = (data_lo + 40, False)
    fails = []
    global PAL_IMAGE_START, PAL_BSS_START, PAL_IMAGE_END
    saved = (PAL_IMAGE_START, PAL_BSS_START, PAL_IMAGE_END)
    PAL_IMAGE_START, PAL_BSS_START, PAL_IMAGE_END = data_lo, bss_lo, bss_lo + 0x100
    try:
        for shift in (0, 0x1000):
            other_bytes = pal.data if not shift else make_shifted(pal, shift)[0]
            other = Elf(other_bytes, "other")
            cen = {}
            doc = make_map(pal, other, symbols, census=cen)
            seg = doc["segments"]
            if bool(doc["pointers"]) != bool(shift):
                fails.append("shift %#x: %d pointer words to translate" % (shift, len(doc["pointers"])))
            if len(seg) != 1 or u32(seg[0]["other"]) - u32(seg[0]["pal"]) != shift or seg[0]["bytes"] != len(data):
                fails.append("shift %#x: segments %r" % (shift, seg))
            if any(u32(o) - u32(p) != shift for o, p in doc["code"]) or len(doc["code"]) != len({struct.unpack_from("<I", data, k)[0] for k in range(0, len(data), 20)}):
                fails.append("shift %#x: code pairs %r" % (shift, doc["code"][:4]))
            if not doc["bss"] or u32(doc["bss"][0]["other"]) - u32(doc["bss"][0]["pal"]) != shift:
                fails.append("shift %#x: bss %r" % (shift, doc["bss"]))
            mem, stats = apply_map(doc, other)
            if mem != native_image(pal):
                fails.append("shift %#x: the rebuilt image differs" % shift)
            if cen["symbols"].get("D_%08X" % (data_lo + 40), {}).get("other") != "0x%08X" % (data_lo + 40 + shift):
                fails.append("shift %#x: symbol map %r" % (shift, cen["symbols"]))
            if doc["coverage"]["differing_bytes"]:
                fails.append("shift %#x: %d differing bytes" % (shift, doc["coverage"]["differing_bytes"]))
        # an insertion in the other's data: the words after it still align (by content), at their new place
        ins = bytearray(data)
        ins[100:100] = b"\x11\x22\x33\x44" * 3
        other = Elf(synthetic_elf(text_lo, [(".text", text_lo, bytes(text)), (".data", data_lo, bytes(ins)),
                                            (".bss", data_lo + len(ins), ("nobits", 0x100))]), "inserted")
        doc = make_map(pal, other, symbols)
        if not any(u32(s["other"]) - u32(s["pal"]) == 12 for s in doc["segments"]):
            fails.append("insertion: no segment moved by 12: %r" % doc["segments"])
        # a string field of another length: the shorter string replaces the longer one (the start rule), the rest zero
        a_ = bytearray(data)
        a_[40:52] = b"Crash_Euro\0\0"
        b_ = bytearray(data)
        b_[40:52] = b"Crash\0\0\0" + b_[52:56]
        del b_[52:56]
        other = Elf(synthetic_elf(text_lo, [(".text", text_lo, bytes(text)), (".data", data_lo, bytes(b_)),
                                            (".bss", data_lo + len(b_), ("nobits", 0x100))]), "shorter")
        pal2 = Elf(synthetic_elf(text_lo, [(".text", text_lo, bytes(text)), (".data", data_lo, bytes(a_)),
                                           (".bss", data_lo + len(a_), ("nobits", 0x100))]), "pal2")
        PAL_BSS_START, PAL_IMAGE_END = data_lo + len(a_), data_lo + len(a_) + 0x100
        doc = make_map(pal2, other, symbols)
        mem, _ = apply_map(doc, other)
        got = mem[40:52]
        if got[:6] != b"Crash\0":
            fails.append("shorter string: %r" % got)
    finally:
        PAL_IMAGE_START, PAL_BSS_START, PAL_IMAGE_END = saved
    print("elf_region_map selftest: %s" % ("PASS" if not fails else "FAIL"))
    for f in fails:
        print("  " + f)
    return 0 if not fails else 1


# ------------------------------------------------------------------------------------------------------------- main
def summary_line(doc, serial, other_exe):
    c = doc["coverage"]
    return ("elf_region_map map: %s %s -> PAL: %d segments (%d of %d data-image bytes: %d by content, %d moved, %d placed), "
            "%d bss ranges, %d pointer words (%d code, %d data, %d placed, %d unmapped; %d code pointers with no PAL function), "
            "%d fills, %d of %d PAL data symbols mapped, %d bytes differ from the PAL image" %
            (serial or "(no serial)", other_exe, len(doc["segments"]), c["aligned_bytes"], c["data_image_bytes"], c["content_bytes"],
             c["moved_bytes"], c["placed_bytes"], len(doc["bss"]), c["pointer_words"], c["code_pointer_words"], c["data_pointer_words"],
             c["data_pointers_placed"], c["data_pointers_unmapped"], c["code_pointers_no_pal_function"], c["fills"],
             c["pal_data_symbols_mapped"], c["pal_data_symbols"], c["differing_bytes"]))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    for name in ("map", "shift", "verify", "census"):
        p = sub.add_parser(name)
        p.add_argument("--pal-elf", default="")
        p.add_argument("--pal-iso", default="")
        p.add_argument("--symbols", default=os.path.join(ROOT, "game", "symbol_addrs.txt"))
        p.add_argument("--retail-image", default="", help="build/wasm/retail_image.json: the symbols the game's objects reference")
        p.add_argument("--out", default="")
        if name in ("map", "verify"):
            p.add_argument("--other-elf", default="")
            p.add_argument("--other-iso", default="")
        if name == "map":
            p.add_argument("--serial", default="")
            p.add_argument("--census-out", default="", help="the tables behind the map's counts (every PAL data symbol, every object)")
            p.add_argument("--engine-map", default="", help="engine/.../symbols/pal_ntsc_map.tsv: compare the function map")
            p.add_argument("--data-names", default="", help="engine/.../symbols/data_symbols.tsv: name the other executable's words")
        if name == "shift":
            p.add_argument("--shift", default="0x1000")
        if name == "verify":
            p.add_argument("--map", required=True)
    sub.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest()
    symbols = read_symbols(a.symbols)
    retail_image = json.load(open(a.retail_image)) if a.retail_image else None
    pal_exe, pal_serial, pal = load_program(a.pal_elf, a.pal_iso, "pal")
    if a.cmd == "census":
        text = json.dumps(census(pal, symbols, retail_image), indent=1) + "\n"
        if a.out:
            open(a.out, "w").write(text)
        sys.stdout.write(text)
        return 0
    if a.cmd == "shift":
        shifted, rebased, ambiguous = make_shifted(pal, int(a.shift, 0))
        if not a.out:
            raise SystemExit("elf_region_map shift: --out FILE (outside the repository: it is the PAL executable, moved)")
        open(a.out, "wb").write(shifted)
        print("elf_region_map shift: %s -- every section +%s, %d pointer words rebased (%d that read as a string's end left alone), %d lui "
              "of the code rebased, %d words in the band past the image (left alone), sha256 %s" %
              (a.out, a.shift, rebased, make_shifted.kept, make_shifted.rebased_lui, ambiguous, hashlib.sha256(shifted).hexdigest()))
        return 0
    other_exe, other_serial, other = load_program(a.other_elf, a.other_iso, "other")
    if a.cmd == "verify":
        doc = json.load(open(a.map))
        if doc.get("elf", {}).get("sha256") != other.sha256():
            print("elf_region_map verify: the map was made for another executable (sha256 %s, this one %s)" %
                  (doc.get("elf", {}).get("sha256"), other.sha256()))
            return 1
        mem, stats = apply_map(doc, other)
        want = native_image(pal)
        diff = sum(1 for x, y in zip(mem, want) if x != y)
        listed = doc.get("coverage", {}).get("differing_bytes", 0)
        print("elf_region_map verify: the PAL data image rebuilt from %s through the map: %d of %d bytes differ from %s's "
              "own (pointers translated: %d code, %d data exceptions, %d by segment, %d unmapped; %d fills; the map lists %d)" %
              (other.name, diff, len(want), pal.name, stats["code"], stats["data"], stats["segment"], stats["unmapped"],
               stats["fills"], listed))
        return 0 if diff == listed else 1
    serial = a.serial or other_serial or ""
    census = {} if a.census_out else None
    doc = make_map(pal, other, symbols, retail_image, read_engine_map(a.engine_map), serial, other_exe, read_data_names(a.data_names),
                   census=census)
    text = json.dumps(doc, indent=1) + "\n"
    print(summary_line(doc, serial, other_exe))
    if a.out:
        os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
        with open(a.out, "w", newline="\n") as f:
            f.write(text)
        print("elf_region_map map: written %s (%d bytes)" % (a.out, len(text)))
    if a.census_out:
        census = dict({"kind": "region-map-census", "serial": serial, "executable": other_exe, "coverage": doc["coverage"]}, **census)
        with open(a.census_out, "w", newline="\n") as f:
            f.write(json.dumps(census, indent=1) + "\n")
        print("elf_region_map map: the census written %s" % a.census_out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
