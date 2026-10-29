#!/usr/bin/env python3
"""mods_import_judge.py -- does the mod importer (twinsanityevolved-modtool, the library the launcher links) turn every
shape the community ships into an overlay folder the game composes exactly, and does the game refuse a recipe over the
wrong base? (fixtures/judges/mods_import_pal.json; every mod is made at run time from the disc and deleted afterwards.)

  (i)   ISO     the text-bank fixture overlay (tools/mods/make_fixture_overlay.py: "graphic options" -> "graphic optionz")
                composed into a copy of the disc (tools/mods/apply_overlay_to_iso.py: the 2.6 GB "patched ISO") ->
                `modtool import` -> the folder EQUIVALENT to the fixture: the same disc files touched, and for each the same
                composed bytes -- `modtool verify` (the game's overlay) and this judge's own recomposition give the patched
                image's sha256 -- and a mod folder of at most `max_mod_bytes`.
  (ii)  XDELTA  an xdelta3 patch of that image pair (the real xdelta3, -S none) imports to the same; a patch made with
                xdelta3's secondary compression (-S djw) is refused with a sentence saying so.
  (iii) PPF/BPS a PPF 3.0 and a BPS patch of the pair (writers below) import to the same.
                The patched image exists once, while the patches are made and (i) runs; it is deleted before (ii).
  (iv)  CRATE   a Mod Crate folder (ModCrateInfo.txt; layer0/SYSTEM.CNF with one byte changed; layer1/<the bank> with the
                fixture's change; layer1/Mods/ImportJudge/added.bin, an added entry that grows the archive past its sectors)
                imports; recomposed, every archive entry is the crate's or the disc's; the game on the OPTIONS route draws
                the changed line and never the original, with the census row counting the layer and the archive moved to
                the virtual region.
  (v)   EXE     a folder holding a copy of the disc's SLES_525.68 with 8 bytes changed inside FUN_001ac8d0
                (Font::ConstructBase) and 4 in a .rodata string the port never reads imports with code_changes naming that
                function and data_changes .rodata 4; with it on the game boots to the Title.
  (vi)  BASE    the (iv) mod with its recipe's base record hand-edited three ways (the sampled sha256; the whole sha256 with
                no sample; the size) is refused at start-up with the log line, and the game runs vanilla (the original line,
                the census row 0 layers, the pictures identical to the vanilla run).
  (vii) LOOSE   a bare archive entry: the disc's own Startup\\Default.rm2, extracted at run time
                with 4 bytes cut from its end (never the original file), dropped as one file named Default.rm2 -> the mod
                `default`: CRASH6/CRASH.BH rebuilt (every later entry 4 bytes earlier) and CRASH6/CRASH.BD a recipe; verify, and
                this judge's own recomposition: that entry is the cut bytes, every other entry the disc's. A bare text bank
                whose name several entries share is refused with them listed. With --ntsc-iso, the same cut file over the
                North American disc is refused: "The file Default.rm2 matches the European disc's entry ...".
  (viii) USA    with --ntsc-iso (SKIPPED otherwise): a Mod Crate folder made at run time from that disc -- ModCrateInfo.txt
                with an "!... apply Classic Slide Jump for Crash and Classic Explosion Damage." line, layer0/crash6/MUSIC.MH
                (lower-case folder, one byte changed), layer1/Language/Code/American.txt ("graphic options" -> "...optionz")
                -- imported against it: base NTSC-U / SLUS_209.09 / its Redump name, the notes kept, the warning that the
                port cannot apply those Crate Mod Loader settings, verify and the recomposition; `inspect` says the same. The
                same crate over --iso (the European disc) is imported with the warning that it looks made for the North
                American disc.

RED on the base (no twinsanityevolved-modtool beside the game: nothing can be imported; and its overlay has no
base check). GREEN on the import build.

Usage: mods_import_judge.py --bin BIN --iso ISO [--ntsc-iso NTSC] [--modtool TOOL] [--xdelta3 XD3] [--out DIR] [--fixture F]
                            [--only iso,xdelta,ppf_bps,crate,exe,base,loose,usa_crate] [--keep]
Exit 0 GREEN, 1 RED, 2 the setup failed. Writes OUT/report.json.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(ROOT, "tools", "mods"))
import apply_overlay_to_iso as overlay  # noqa: E402

DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "mods_import_pal.json")
STRING = re.compile(r"\[te\.gfx\.diag\] present \d+ text \d+ quads .* string '(.*)'$")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")
CENSUS = re.compile(r"^\[twinsanityevolved\] mods=(\d+):(\d+):(\d+)\s*$", re.M)
CHUNK = 1 << 22


def log(msg):
    print("mods_import_judge: %s" % msg, flush=True)


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(CHUNK)
            if not b:
                return h.hexdigest()
            h.update(b)


def modtool(tool, args, timeout=3600):
    cmd = [tool] + args + ["--json"]
    log("modtool " + " ".join(args))
    p = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True, timeout=timeout)
    try:
        out = json.loads(p.stdout)
    except ValueError:
        out = {"ok": False, "error": (p.stderr or p.stdout).strip()[-400:]}
    out["rc"] = p.returncode
    return out


def run_game(binary, iso, out, tag, padlog, stop_at, extra, env_extra, run_seconds, harness=False):
    d = os.path.join(out, "runs", tag)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "mc"))
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, padlog), "--stop-at", str(stop_at), "--mc-dir", os.path.join(d, "mc"),
            "--run-seconds", str(run_seconds), "--config", "none"] + extra
    if harness:
        args += ["--harness-out", os.path.join(d, "harness")]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.update(env_extra)
    log("run %s: %s" % (tag, " ".join(args[1:])))
    with open(os.path.join(d, "run.log"), "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    with open(os.path.join(d, "run.log"), errors="replace") as f:
        text = f.read()
    log("run %s exit %d" % (tag, rc))
    return {"rc": rc, "text": text, "dir": d}


def strings(text):
    counts = {}
    for line in text.splitlines():
        m = STRING.match(line)
        if m:
            s = m.group(1).lower()
            counts[s] = counts.get(s, 0) + 1
    return counts


def census(text):
    rows = CENSUS.findall(text)
    return tuple(int(v) for v in rows[-1]) if rows else None


# ---- patch writers over two images (equal-offset differences: what an in-place patched image has) ------------------------
def differences(a_path, b_path):
    """(offset, bytes of b) runs where b differs from a (and b's tail past a); with both images' CRC-32s."""
    runs, crc_a, crc_b, at = [], 0, 0, 0
    with open(a_path, "rb") as fa, open(b_path, "rb") as fb:
        while True:
            x, y = fa.read(CHUNK), fb.read(CHUNK)
            if not x and not y:
                break
            crc_a, crc_b = zlib.crc32(x, crc_a), zlib.crc32(y, crc_b)
            if x != y:
                n = min(len(x), len(y))
                k = 0
                while k < n:
                    if x[k] == y[k]:
                        k += 1
                        continue
                    j = k
                    while j < n and x[j] != y[j]:
                        j += 1
                    runs.append((at + k, y[k:j]))
                    k = j
                if len(y) > len(x):
                    runs.append((at + len(x), y[len(x):]))
            at += len(y) if y else len(x)
    return runs, crc_a & 0xFFFFFFFF, crc_b & 0xFFFFFFFF


def write_ppf3(path, src_path, runs):
    with open(src_path, "rb") as f:
        f.seek(0x9320)
        block = f.read(1024)
    with open(path, "wb") as p:
        p.write(b"PPF30" + bytes([2]) + b"mods import judge".ljust(50, b" ") + bytes([0, 1, 0, 0]) + block)
        for off, data in runs:
            for k in range(0, len(data), 255):
                piece = data[k:k + 255]
                p.write(struct.pack("<QB", off + k, len(piece)) + piece)


def bps_num(v):
    out = bytearray()
    while True:
        x = v & 0x7F
        v >>= 7
        if v == 0:
            out.append(0x80 | x)
            return bytes(out)
        out.append(x)
        v -= 1


def write_bps(path, src_size, dst_size, runs, crc_src, crc_dst):
    body = bytearray(b"BPS1" + bps_num(src_size) + bps_num(dst_size) + bps_num(0))
    at = 0
    for off, data in runs:
        if off > at:
            body += bps_num(((off - at - 1) << 2) | 0)   # SourceRead
        body += bps_num(((len(data) - 1) << 2) | 1) + data   # TargetRead
        at = off + len(data)
    if dst_size > at:
        body += bps_num(((dst_size - at - 1) << 2) | 0)
    body += struct.pack("<II", crc_src, crc_dst)
    body += struct.pack("<I", zlib.crc32(bytes(body)) & 0xFFFFFFFF)
    with open(path, "wb") as p:
        p.write(body)


def vcd_int(v):
    out = [v & 0x7F]
    v >>= 7
    while v:
        out.append(0x80 | (v & 0x7F))
        v >>= 7
    return bytes(reversed(out))


def write_vcdiff(path, src_path, dst_path, window=1 << 24):
    """A VCDIFF (RFC 3284, xdelta3's Adler-32) of an in-place image pair, for a machine without xdelta3: per window the
    source segment is the same range of the source, equal runs COPY it (mode SELF), the rest is ADDed."""
    with open(src_path, "rb") as fs, open(dst_path, "rb") as fd, open(path, "wb") as p:
        p.write(bytes([0xD6, 0xC3, 0xC4, 0x00, 0x00]))
        at = 0
        while True:
            t = fd.read(window)
            if not t:
                break
            sseg = fs.read(len(t))
            data, inst, addr = bytearray(), bytearray(), bytearray()
            k = 0
            n = min(len(t), len(sseg))
            while k < len(t):
                j = k
                while j + 65536 <= n and t[j:j + 65536] == sseg[j:j + 65536]:
                    j += 65536
                while j < n and t[j] == sseg[j]:
                    j += 1
                if j - k >= 8:
                    inst += bytes([19]) + vcd_int(j - k)   # COPY size 0, mode SELF
                    addr += vcd_int(k)
                    k = j
                    continue
                j = k
                while j < len(t) and not (j + 8 <= len(sseg) and j + 8 <= len(t) and t[j:j + 8] == sseg[j:j + 8]):
                    j += 1
                inst += bytes([1]) + vcd_int(j - k)        # ADD size 0
                data += t[k:j]
                k = j
            enc = vcd_int(len(t)) + bytes([0]) + vcd_int(len(data)) + vcd_int(len(inst)) + vcd_int(len(addr)) + \
                struct.pack(">I", zlib.adler32(t) & 0xFFFFFFFF) + bytes(data) + bytes(inst) + bytes(addr)
            p.write(bytes([0x01 | 0x04]) + vcd_int(len(sseg)) + vcd_int(at) + vcd_int(len(enc)) + enc)
            at += len(t)


def mod_hashes(disc, mod_dir):
    """{disc path: composed sha256} of every file a mod folder touches, recomposed here (not by the tool)."""
    return {path: overlay.composed_sha256(disc, mod_dir, path) for path in sorted(overlay.mod_files(mod_dir))}


def folder_bytes(d):
    return sum(os.path.getsize(os.path.join(dp, n)) for dp, _, ns in os.walk(d) for n in ns)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--modtool", default="", help="default: twinsanityevolved-modtool beside --bin")
    ap.add_argument("--xdelta3", default="", help="default: $XDELTA3, else xdelta3 on PATH")
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "mods_import_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--ntsc-iso", default="", help="the North American disc (SLUS-20909): (vii)'s refusal and (viii); SKIPPED without it")
    ap.add_argument("--only", default="", help="run only these checks (comma separated); the verdict counts them alone")
    ap.add_argument("--keep", action="store_true")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    binary, iso = os.path.abspath(a.bin), os.path.abspath(a.iso)
    tool = os.path.abspath(a.modtool) if a.modtool else os.path.join(os.path.dirname(binary), "twinsanityevolved-modtool")
    xd3 = a.xdelta3 or os.environ.get("XDELTA3") or shutil.which("xdelta3") or ""
    out = os.path.abspath(a.out)
    shutil.rmtree(out, ignore_errors=True)
    os.makedirs(out)
    mods = os.path.join(out, "mods")
    report = {"bin": binary, "modtool": tool, "iso": iso, "xdelta3": xd3, "fixture": os.path.relpath(a.fixture, ROOT), "checks": {}}
    patched = os.path.join(out, "patched.iso")

    def finish(code, line):
        report["verdict"] = line
        for big in (patched,):
            if os.path.exists(big):
                os.remove(big)
        shutil.rmtree(os.path.join(mods, ".import-tmp"), ignore_errors=True)
        with open(os.path.join(out, "report.json"), "w") as f:
            json.dump(report, f, indent=2)
        log(line)
        if not a.keep:
            for name in os.listdir(out):
                p = os.path.join(out, name)
                if os.path.isdir(p) and name != "runs":
                    shutil.rmtree(p, ignore_errors=True)
                elif name.endswith((".xdelta", ".ppf", ".bps", ".bin")):
                    os.remove(p)
        return code

    checks = report["checks"]
    names = ["iso", "xdelta", "ppf_bps", "crate", "exe", "base", "loose", "usa_crate"]
    if a.only:
        wanted = [n for n in a.only.split(",") if n]
        unknown = [n for n in wanted if n not in names]
        if unknown:
            return finish(2, "SETUP FAILED -- --only names no check %s (the checks: %s)" % (unknown, ",".join(names)))
        names = [n for n in names if n in wanted]
        report["only"] = names
    ntsc = os.path.abspath(a.ntsc_iso) if a.ntsc_iso else ""
    report["ntsc_iso"] = ntsc
    if not os.path.isfile(tool) or not os.access(tool, os.X_OK):
        for n in names:
            checks[n] = {"result": "RED: no twinsanityevolved-modtool beside the game (%s): nothing can be imported" % tool}
        return finish(1, "RED -- no mod importer: " + "; ".join("%s RED" % n for n in names))
    disc = overlay.Disc(iso)
    run_s = fx["run_seconds"]
    old_names = [n for n in names if n in ("iso", "xdelta", "ppf_bps", "crate", "exe", "base")]
    if old_names:
        r = judge_formats(a, fx, report, checks, tool, xd3, out, mods, patched, binary, iso, disc, run_s, old_names, finish)
        if r is not None:
            return r
    if "loose" in names:
        checks["loose"] = judge_loose(fx["loose"], tool, out, mods, iso, disc, ntsc)
    if "usa_crate" in names:
        checks["usa_crate"] = judge_usa_crate(fx["usa_crate"], tool, out, mods, iso, ntsc)
    red = [n for n in names if checks.get(n, {}).get("result") not in ("GREEN", "SKIPPED")]
    summary = "; ".join("%s %s" % (n, checks.get(n, {}).get("result", "RED")[:7] if n not in red else "RED") for n in names)
    if red:
        return finish(1, "RED -- " + summary + " | " + " | ".join("%s: %s" % (n, str(checks.get(n, {}).get("result"))[:300]) for n in red))
    return finish(0, "GREEN -- " + summary)


def judge_formats(a, fx, report, checks, tool, xd3, out, mods, patched, binary, iso, disc, run_s, names, finish):
    """(i)..(vi): None when they ran (their results in `checks`), else the setup failure's exit code."""
    # ---- the fixture overlay and the patched image ------------------------------------------------------------------------
    fixdir = os.path.join(out, "fixture")
    gen = subprocess.run([sys.executable, os.path.join(ROOT, fx["generator"]), "--iso", iso, "--out", fixdir, "--kinds", "textbank"],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    if gen.returncode != 0:
        return finish(2, "SETUP FAILED -- the fixture generator: %s" % gen.stdout.strip()[-300:])
    with open(os.path.join(fixdir, "report.json")) as f:
        made = json.load(f)["mods"]["textbank"]
    fixture_mod = os.path.join(fixdir, "textbank")
    want = mod_hashes(disc, fixture_mod)          # the fixture's composed files, recomposed here
    p = subprocess.run([sys.executable, os.path.join(ROOT, fx["patcher"]), "--iso", iso, "--mod", fixture_mod, "--out", patched],
                       stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    if p.returncode != 0:
        return finish(2, "SETUP FAILED -- the patched image: %s" % p.stdout.strip()[-300:])
    pdisc = overlay.Disc(patched)
    for path, h in want.items():
        if overlay.disc_sha256(pdisc, path) != h:
            return finish(2, "SETUP FAILED -- the patched image's %s is not the fixture's composition" % path)
    report["fixture"] = {"files": want, "change": "%s -> %s in %s" % (made["from"], made["to"], [b["bank"] for b in made["banks"]])}
    # the patches of the pair, made while the image exists
    runs, crc_a, crc_b = differences(iso, patched)
    src_size, dst_size = os.path.getsize(iso), os.path.getsize(patched)
    patches = {}
    write_ppf3(os.path.join(out, "image.ppf"), iso, runs)
    patches["ppf"] = os.path.join(out, "image.ppf")
    write_bps(os.path.join(out, "image.bps"), src_size, dst_size, runs, crc_a, crc_b)
    patches["bps"] = os.path.join(out, "image.bps")
    if xd3:
        dst = os.path.join(out, "image.xdelta")
        r = subprocess.run([xd3, "-e", "-f", "-S", "none", "-s", iso, patched, dst], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0:
            patches["xdelta"] = dst
        # xdelta3 compresses a window's sections a second time only where that pays: a pair with a large compressible
        # change (the disc's first 8 MB, 1 MB of it text) makes a -S djw patch whose windows do use it
        src8, dst8 = os.path.join(out, "djw_src.bin"), os.path.join(out, "djw_dst.bin")
        with open(iso, "rb") as f:
            head = bytearray(f.read(8 << 20))
        with open(src8, "wb") as f:
            f.write(head)
        text = b"".join(b"line %06d of the secondary compression test\n" % i for i in range(25000))[:1 << 20]
        head[1 << 20:(1 << 20) + len(text)] = text
        with open(dst8, "wb") as f:
            f.write(head)
        dst = os.path.join(out, "djw.xdelta")
        r = subprocess.run([xd3, "-e", "-f", "-S", "djw", "-s", src8, dst8, dst], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        if r.returncode == 0:
            patches["xdelta_djw"] = dst
        for tmp in (src8, dst8):
            os.remove(tmp)
    else:   # no xdelta3 here: the judge's own VCDIFF of the pair (the refusal of secondary compression is then not run)
        write_vcdiff(os.path.join(out, "image.xdelta"), iso, patched)
        patches["xdelta"] = os.path.join(out, "image.xdelta")
        report["xdelta3"] = "none: the judge's own VCDIFF writer"
    report["patches"] = {k: os.path.getsize(v) for k, v in patches.items()}
    report["differences"] = {"runs": len(runs), "bytes": sum(len(d) for _, d in runs)}

    def same_as_fixture(res, label):
        if res.get("rc") != 0 or not res.get("ok"):
            return "RED: %s import failed: %s" % (label, res.get("error"))
        mod_dir = os.path.join(mods, res["id"])
        got = mod_hashes(disc, mod_dir)
        v = modtool(tool, ["verify", mod_dir, "--iso", iso])
        entry = {"id": res["id"], "files": got, "verify": v.get("ok"), "mod_bytes": folder_bytes(mod_dir)}
        if not v.get("ok"):
            return "RED: %s: modtool verify fails: %s" % (label, v.get("error"))
        if got != want:
            return "RED: %s: composes %s, the fixture %s" % (label, got, want)
        tool_rows = {r["path"]: r["sha256"] for r in v.get("files", [])}
        if any(tool_rows.get(p) != h for p, h in want.items()):
            return "RED: %s: the overlay composes %s" % (label, tool_rows)
        if entry["mod_bytes"] > fx["iso"]["max_mod_bytes"]:
            return "RED: %s: the mod folder is %d bytes (> %d)" % (label, entry["mod_bytes"], fx["iso"]["max_mod_bytes"])
        return entry

    # ---- (i) the patched image
    res = modtool(tool, ["import", patched, "--iso", iso, "--mods", mods])
    r = same_as_fixture(res, "the image")
    checks["iso"] = {"result": r} if isinstance(r, str) else dict(r, result="GREEN")
    os.remove(patched)   # the one 2.6 GB temporary: gone before the patches are applied (each makes its own, under mods/)
    # ---- (ii) xdelta3
    if "xdelta" not in patches:
        checks["xdelta"] = {"result": "RED: the xdelta3 patch was not made"}
    else:
        res = modtool(tool, ["import", patches["xdelta"], "--iso", iso, "--mods", mods])
        r = same_as_fixture(res, "the xdelta patch")
        entry = {"result": r} if isinstance(r, str) else dict(r, result="GREEN")
        if "xdelta_djw" in patches and entry["result"] == "GREEN":
            refused = modtool(tool, ["import", patches["xdelta_djw"], "--iso", iso, "--mods", mods])
            entry["secondary_refused"] = refused.get("error")
            if refused.get("ok") or "secondary compression" not in (refused.get("error") or ""):
                entry["result"] = "RED: an xdelta3 -S djw patch was not refused with a sentence about secondary compression: %s" % refused
        checks["xdelta"] = entry
    # ---- (iii) PPF, BPS
    entry = {}
    for k in ("ppf", "bps"):
        res = modtool(tool, ["import", patches[k], "--iso", iso, "--mods", mods])
        r = same_as_fixture(res, "the %s patch" % k.upper())
        entry[k] = {"result": r} if isinstance(r, str) else dict(r, result="GREEN")
    entry["result"] = "GREEN" if all(entry[k]["result"] == "GREEN" for k in ("ppf", "bps")) else \
        "RED: " + "; ".join("%s %s" % (k, entry[k]["result"]) for k in ("ppf", "bps") if entry[k]["result"] != "GREEN")
    checks["ppf_bps"] = entry
    leftovers = os.path.join(mods, ".import-tmp")
    if os.path.exists(leftovers):
        checks["ppf_bps"]["result"] = "RED: the importer left %s behind" % leftovers

    # ---- (iv) a Mod Crate folder ------------------------------------------------------------------------------------------
    cfx = fx["crate"]
    crate = os.path.join(out, "crate")
    bank = made["banks"][0]
    bank_name = bank["bank"]
    bh = disc.read("CRASH6/CRASH.BH", 0, disc.files["CRASH6/CRASH.BH"]["size"])
    entries, at = [], 4
    while at < len(bh):
        n = struct.unpack_from("<i", bh, at)[0]
        name = bh[at + 4:at + 4 + n].decode("latin-1")
        off, size = struct.unpack_from("<II", bh, at + 4 + n)
        entries.append((name, off, size))
        at += 12 + n
    sizes = {e[0]: (e[1], e[2]) for e in entries}
    off, size = sizes[bank_name]
    changed_bank = bytearray(disc.read("CRASH6/CRASH.BD", off, size))
    for b in made["banks"]:
        if b["bank"] == bank_name:
            changed_bank[b["line_offset"]:b["line_offset"] + len(made["to"])] = made["to"].encode("latin-1")
    cnf = disc.read("SYSTEM.CNF", 0, disc.files["SYSTEM.CNF"]["size"])
    cnf2 = cnf.replace(cfx["system_cnf"]["from"].encode(), cfx["system_cnf"]["to"].encode(), 1)
    added = bytes((i * 131 + 7) & 0xFF for i in range(cfx["added_entry_bytes"]))
    os.makedirs(os.path.join(crate, "layer0"))
    with open(os.path.join(crate, "ModCrateInfo.txt"), "w", newline="\r\n") as f:
        f.write("Name=Import Judge Crate\nDescription=the mod import judge's crate\nAuthor=the judge\nVersion=v1\n!made at run time\nModLoaderVersion=v1.3.0\nGame=CrashTS\n")
    with open(os.path.join(crate, "layer0", "SYSTEM.CNF"), "wb") as f:
        f.write(cnf2)
    bank_host = os.path.join(crate, "layer1", *bank_name.split("\\"))
    os.makedirs(os.path.dirname(bank_host))
    with open(bank_host, "wb") as f:
        f.write(bytes(changed_bank))
    added_host = os.path.join(crate, "layer1", "Mods", "ImportJudge", "added.bin")
    os.makedirs(os.path.dirname(added_host))
    with open(added_host, "wb") as f:
        f.write(added)
    res = modtool(tool, ["import", crate, "--iso", iso, "--mods", mods])
    entry = {"import_ok": res.get("ok"), "id": res.get("id"), "files_changed": res.get("files_changed")}
    crate_id = res.get("id")
    if not res.get("ok"):
        entry["result"] = "RED: the crate import failed: %s" % res.get("error")
    else:
        mod_dir = os.path.join(mods, crate_id)
        v = modtool(tool, ["verify", mod_dir, "--iso", iso])
        # recomposed here: the index and the archive, entry by entry
        _, bh_chunks = overlay.compose(disc, mod_dir, "CRASH6/CRASH.BH")
        new_bh = b"".join(bh_chunks)
        new_entries, at = [], 4
        while at < len(new_bh):
            n = struct.unpack_from("<i", new_bh, at)[0]
            name = new_bh[at + 4:at + 4 + n].decode("latin-1")
            o, s = struct.unpack_from("<II", new_bh, at + 4 + n)
            new_entries.append((name, o, s))
            at += 12 + n
        bd_size, bd_chunks = overlay.compose(disc, mod_dir, "CRASH6/CRASH.BD")
        composed_bd = os.path.join(out, "composed_bd.bin")
        with open(composed_bd, "wb") as f:
            for b in bd_chunks:
                f.write(b)
        bad = []
        with open(composed_bd, "rb") as f:
            for name, o, s in new_entries:
                f.seek(o)
                got = f.read(s)
                if name.upper() == bank_name.upper():
                    ok = got == bytes(changed_bank)
                elif name.upper() == "MODS\\IMPORTJUDGE\\ADDED.BIN":
                    ok = got == added
                else:
                    bo, bs = sizes[name]
                    ok = bs == s and got == disc.read("CRASH6/CRASH.BD", bo, bs)
                if not ok:
                    bad.append(name)
        os.remove(composed_bd)
        _, cnf_chunks = overlay.compose(disc, mod_dir, "SYSTEM.CNF")
        cnf_ok = b"".join(cnf_chunks) == cnf2
        entry.update({"verify": v.get("ok"), "entries": len(new_entries), "entries_bad": bad, "system_cnf": cnf_ok,
                      "archive_bytes": bd_size})
        g = run_game(binary, iso, out, "crate", cfx["padlog"], cfx["stop_at"], ["--mods-dir", mods, "--mod", crate_id],
                     {"TE_GFX_DIAG_TEXT": "1"}, run_s)
        s = strings(g["text"])
        original, changed = cfx["string"].lower(), made["to"].lower()
        row = census(g["text"])
        moved = re.search(r"mods: disc CRASH6/CRASH\.BD: ranges.*grown: moved to lba \d+ \(virtual\)", g["text"]) is not None
        entry.update({"rc": g["rc"], "changed_draws": s.get(changed, 0), "original_draws": s.get(original, 0), "census": row,
                      "archive_moved": moved})
        ok = v.get("ok") and not bad and len(new_entries) == len(entries) + 1 and cnf_ok and g["rc"] == 0 and \
            s.get(changed, 0) >= cfx["min_draws"] and s.get(original, 0) == 0 and row is not None and row[0] == 1 and \
            row[1] == cfx["overrides"] and row[2] > 0 and moved
        entry["result"] = "GREEN" if ok else "RED: %s" % {k: entry.get(k) for k in ("verify", "entries", "entries_bad", "system_cnf",
                                                                                     "rc", "changed_draws", "original_draws",
                                                                                     "census", "archive_moved")}
    checks["crate"] = entry

    # ---- (v) a modified executable -----------------------------------------------------------------------------------------
    efx = fx["exe"]
    elf = bytearray(disc.read("SLES_525.68", 0, disc.files["SLES_525.68"]["size"]))
    code_at = int(efx["code_address"], 16)
    for k in range(efx["code_bytes"]):
        elf[0x1000 + code_at - 0x100000 + k] ^= 0xA5
    ro_off, ro_addr = 0x1ED400, 0x2EC400
    hit = elf.find(efx["rodata_string"].encode(), ro_off, ro_off + 0x1D468)
    entry = {}
    if hit < 0:
        checks["exe"] = {"result": "RED: the .rodata string %r is not in the disc's program" % efx["rodata_string"]}
    else:
        rodata_addr = ro_addr + (hit - ro_off)
        elf[hit:hit + efx["rodata_bytes"]] = bytes(elf[hit:hit + efx["rodata_bytes"]]).upper()
        exe_dir = os.path.join(out, "exe_mod")
        os.makedirs(exe_dir)
        with open(os.path.join(exe_dir, "SLES_525.68"), "wb") as f:
            f.write(bytes(elf))
        res = modtool(tool, ["import", exe_dir, "--iso", iso, "--mods", mods])
        entry = {"import_ok": res.get("ok"), "id": res.get("id"), "code_changes": res.get("code_changes"),
                 "code_changes_unsupported": res.get("code_changes_unsupported"), "rodata_address": hex(rodata_addr)}
        if not res.get("ok"):
            entry["result"] = "RED: the import failed: %s" % res.get("error")
        else:
            with open(os.path.join(mods, res["id"], "mod.json")) as f:
                m = json.load(f)
            ex = m.get("executable", {})
            entry["data_changes"] = ex.get("data_changes")
            entry["mod_json_code_changes"] = ex.get("code_changes")
            g = run_game(binary, iso, out, "exe", efx["padlog"], efx["stop_at"], ["--mods-dir", mods, "--mod", res["id"]], {}, run_s)
            states = [name for _, name, _ in STATE.findall(g["text"])]
            composed = re.search(r"mods: disc SLES_525\.68: replace", g["text"]) is not None
            entry.update({"rc": g["rc"], "states": states[-3:], "composed": composed, "census": census(g["text"])})
            want_data = {".data": 0, ".rodata": efx["rodata_bytes"], ".sdata": 0, "vu": 0}
            ok = res.get("code_changes") == [efx["function"]] and ex.get("code_changes") == [efx["function"]] and \
                res.get("code_changes_unsupported") is True and ex.get("data_changes") == want_data and g["rc"] == 0 and \
                efx["title_state"] in states and composed
            entry["result"] = "GREEN" if ok else "RED: %s" % {k: entry.get(k) for k in ("code_changes", "data_changes", "rc", "states", "composed")}
        checks["exe"] = entry

    # ---- (vi) a recipe over another base ---------------------------------------------------------------------------------
    bfx = fx["base"]
    entry = {}
    if not crate_id or checks["crate"].get("import_ok") is not True:
        checks["base"] = {"result": "RED: no crate mod to edit (iv failed)"}
    else:
        vanilla = run_game(binary, iso, out, "base_vanilla", bfx["padlog"], bfx["stop_at"], [], {"TE_GFX_DIAG_TEXT": "1"}, run_s, harness=True)
        vs = strings(vanilla["text"])
        original, changed = cfx["string"].lower(), made["to"].lower()
        if vanilla["rc"] != 0 or vs.get(original, 0) < cfx["min_draws"]:
            return finish(2, "SETUP FAILED -- the vanilla OPTIONS run (exit %d, '%s' x%d)" % (vanilla["rc"], original, vs.get(original, 0)))
        with open(os.path.join(mods, crate_id, "mod.json")) as f:
            manifest = json.load(f)
        all_ok = True
        for variant in bfx["variants"]:
            vid = "mismatch_" + variant
            vdir = os.path.join(mods, vid)
            shutil.copytree(os.path.join(mods, crate_id), vdir)
            m = json.loads(json.dumps(manifest))
            m["id"] = vid
            row = next(r for r in m["files"] if r["path"] == "CRASH6/CRASH.BD")
            if variant == "sample":
                row["base_sample_sha256"] = "0" * 64
            elif variant == "whole":
                row["base_sample_sha256"] = ""
                row["base_sha256"] = "f" * 64
            else:
                row["base_bytes"] = row["base_bytes"] + 1
            with open(os.path.join(vdir, "mod.json"), "w") as f:
                json.dump(m, f, indent=2)
            g = run_game(binary, iso, out, "base_" + variant, bfx["padlog"], bfx["stop_at"], ["--mods-dir", mods, "--mod", vid],
                         {"TE_GFX_DIAG_TEXT": "1"}, run_s, harness=True)
            s = strings(g["text"])
            line = re.search(r"mods: the disc overlay left out %s: disc/CRASH6/CRASH\.BD\.ranges: made for another CRASH6/CRASH\.BD[^\n]*" % vid, g["text"])
            same = {}
            for fname in bfx["identical_files"]:
                pa, pb = os.path.join(vanilla["dir"], "harness", fname), os.path.join(g["dir"], "harness", fname)
                same[fname] = os.path.isfile(pa) and os.path.isfile(pb) and open(pa, "rb").read() == open(pb, "rb").read()
            row_c = census(g["text"])
            ok = g["rc"] == 0 and line is not None and s.get(changed, 0) == 0 and s.get(original, 0) == vs.get(original, 0) and \
                row_c is not None and row_c[0] == 0 and all(same.values())
            entry[variant] = {"rc": g["rc"], "log": line.group(0) if line else None, "original_draws": s.get(original, 0),
                              "changed_draws": s.get(changed, 0), "census": row_c, "identical_to_vanilla": same,
                              "result": "GREEN" if ok else "RED"}
            all_ok = all_ok and ok
            shutil.rmtree(vdir, ignore_errors=True)
        entry["vanilla_original_draws"] = vs.get(original, 0)
        entry["result"] = "GREEN" if all_ok else "RED: %s" % {k: v["result"] for k, v in entry.items() if isinstance(v, dict)}
        checks["base"] = entry

    return None


# ---- (vii) a loose archive entry -------------------------------------------------------------------------------------------
def archive_index(data):
    out, at = [], 4
    while at < len(data):
        n = struct.unpack_from("<i", data, at)[0]
        name = data[at + 4:at + 4 + n].decode("latin-1")
        o, sz = struct.unpack_from("<II", data, at + 4 + n)
        out.append((name, o, sz))
        at += 12 + n
    return out


def disc_entries(disc):
    return archive_index(disc.read("CRASH6/CRASH.BH", 0, disc.files["CRASH6/CRASH.BH"]["size"]))


def entry_bytes(disc, entries, name):
    for n, o, sz in entries:
        if n.upper() == name.upper():
            return disc.read("CRASH6/CRASH.BD", o, sz)
    return None


def recomposed_archive(disc, mod_dir, out, expect):
    """The mod's index + archive recomposed by this judge (tools/mods/apply_overlay_to_iso.py, not the tool): the entries
    whose bytes are not `expect`'s (name -> bytes) or, for the rest, the disc's; and the composed index's entries."""
    _, bh_chunks = overlay.compose(disc, mod_dir, "CRASH6/CRASH.BH")   # (None, None): the mod keeps the disc's index
    new_entries = archive_index(b"".join(bh_chunks)) if bh_chunks is not None else disc_entries(disc)
    base = {n.upper(): (o, sz) for n, o, sz in disc_entries(disc)}
    size, bd_chunks = overlay.compose(disc, mod_dir, "CRASH6/CRASH.BD")
    if bd_chunks is None:
        return new_entries, ["the mod leaves CRASH6/CRASH.BD as the disc's"], disc.files["CRASH6/CRASH.BD"]["size"]
    tmp = os.path.join(out, "composed_bd.bin")
    with open(tmp, "wb") as f:
        for b in bd_chunks:
            f.write(b)
    bad = []
    try:
        with open(tmp, "rb") as f:
            for name, o, sz in new_entries:
                f.seek(o)
                got = f.read(sz)
                want = expect.get(name.upper())
                if want is None:
                    bo, bs = base.get(name.upper(), (None, None))
                    ok = bo is not None and bs == sz and got == disc.read("CRASH6/CRASH.BD", bo, bs)
                else:
                    ok = got == want
                if not ok:
                    bad.append(name)
    finally:
        os.remove(tmp)
    return new_entries, bad, size


def judge_loose(lfx, tool, out, mods, iso, disc, ntsc):
    entries = disc_entries(disc)
    whole = entry_bytes(disc, entries, lfx["entry"])
    if whole is None:
        return {"result": "RED: the disc has no %s" % lfx["entry"]}
    cut = whole[:len(whole) - lfx["cut_bytes"]]
    d = os.path.join(out, "loose")
    os.makedirs(d, exist_ok=True)
    bare = os.path.join(d, lfx["file"])
    with open(bare, "wb") as f:
        f.write(cut)
    e = {"entry": lfx["entry"], "entry_bytes": len(whole), "file_bytes": len(cut)}
    try:
        det = modtool(tool, ["detect", bare])
        e["detected"] = det.get("detected")
        res = modtool(tool, ["import", bare, "--iso", iso, "--mods", mods])
        e.update({"import_ok": res.get("ok"), "id": res.get("id"), "files_changed": res.get("files_changed"), "error": res.get("error")})
        if not res.get("ok"):
            e["result"] = "RED: the loose %s was not imported: %s" % (lfx["file"], res.get("error"))
            return e
        mod_dir = os.path.join(mods, res["id"])
        with open(os.path.join(mod_dir, "mod.json")) as f:
            m = json.load(f)
        v = modtool(tool, ["verify", mod_dir, "--iso", iso])
        new_entries, bad, bd_size = recomposed_archive(disc, mod_dir, out, {lfx["entry"].upper(): cut})
        moved = [n for (n, o, _), (n2, o2, _) in zip(entries, new_entries) if n == n2 and o2 != o]
        e.update({"verify": v.get("ok"), "entries": len(new_entries), "entries_bad": bad, "archive_bytes": bd_size,
                  "entries_moved": len(moved), "source": m.get("source"),
                  "entries_changed": ((m.get("import") or {}).get("archive") or {}).get("entries_changed")})
        ok = res.get("id") == lfx["id"] and sorted(res.get("files_changed") or []) == sorted(lfx["files_changed"]) and v.get("ok") and \
            not bad and len(new_entries) == len(entries) and bd_size == disc.files["CRASH6/CRASH.BD"]["size"] - lfx["cut_bytes"] and \
            (m.get("source") or {}).get("kind") == "folder" and (m.get("source") or {}).get("artefact") == lfx["file"] and \
            e["entries_changed"] == [lfx["entry"]] and len(moved) > 0
        # a bare text bank whose name several entries share: refused, the entries listed
        amb = lfx["ambiguous"]
        bank = entry_bytes(disc, entries, amb["entry"])
        if bank is None:
            e["ambiguous"] = "the disc has no %s" % amb["entry"]
            ok = False
        else:
            bfile = os.path.join(d, amb["file"])
            with open(bfile, "wb") as f:
                f.write(bank)
            r2 = modtool(tool, ["import", bfile, "--iso", iso, "--mods", mods])
            e["ambiguous"] = r2.get("error")
            ok = ok and not r2.get("ok") and (r2.get("error") or "").startswith(amb["refusal_starts"]) and amb["entry"] in (r2.get("error") or "")
        # over the North American disc: another release's entry, refused
        if ntsc:
            r3 = modtool(tool, ["import", bare, "--iso", ntsc, "--mods", os.path.join(out, "mods_ntsc")])
            e["other_release"] = r3.get("error")
            ok = ok and not r3.get("ok") and (r3.get("error") or "").startswith(lfx["other_release"]["refusal_starts"]) and \
                (r3.get("error") or "").endswith(lfx["other_release"]["refusal_ends"])
            shutil.rmtree(os.path.join(out, "mods_ntsc"), ignore_errors=True)
        else:
            e["other_release"] = "SKIPPED: no --ntsc-iso"
        e["result"] = "GREEN" if ok else "RED: %s" % {k: e.get(k) for k in ("id", "files_changed", "verify", "entries", "entries_bad", "archive_bytes",
                                                                             "entries_moved", "source", "entries_changed", "ambiguous", "other_release")}
        return e
    finally:
        shutil.rmtree(d, ignore_errors=True)


# ---- (viii) a USA Mod Crate made from the North American disc ---------------------------------------------------------------
def judge_usa_crate(ufx, tool, out, mods, iso, ntsc):
    if not ntsc:
        return {"result": "SKIPPED", "why": "no --ntsc-iso: the crate is made from the North American disc"}
    ndisc = overlay.Disc(ntsc)
    entries = disc_entries(ndisc)
    bank = entry_bytes(ndisc, entries, ufx["bank"])
    if bank is None or ufx["from"].encode() not in bank:
        return {"result": "RED: the North American disc has no %s with %r" % (ufx["bank"], ufx["from"])}
    bank2 = bank.replace(ufx["from"].encode(), ufx["to"].encode(), 1)
    mh = bytearray(ndisc.read("CRASH6/MUSIC.MH", 0, ndisc.files["CRASH6/MUSIC.MH"]["size"]))
    mh[ufx["music_mh_byte"]] ^= 0x5A
    crate = os.path.join(out, "usa_crate")
    shutil.rmtree(crate, ignore_errors=True)
    os.makedirs(os.path.join(crate, "layer0", "crash6"))   # the lower-case folder of the real USA crates
    with open(os.path.join(crate, "ModCrateInfo.txt"), "w", newline="\r\n") as f:
        f.write(ufx["info"])
    with open(os.path.join(crate, "layer0", "crash6", "MUSIC.MH"), "wb") as f:
        f.write(bytes(mh))
    bank_host = os.path.join(crate, "layer1", *ufx["bank"].split("\\"))
    os.makedirs(os.path.dirname(bank_host))
    with open(bank_host, "wb") as f:
        f.write(bank2)
    e = {}
    try:
        ins = modtool(tool, ["inspect", crate, "--iso", ntsc])
        e["inspect"] = {k: ins.get(k) for k in ("ok", "name", "notes", "warnings", "files_changed", "archive_entries_changed")}
        res = modtool(tool, ["import", crate, "--iso", ntsc, "--mods", mods])
        e.update({"import_ok": res.get("ok"), "id": res.get("id"), "files_changed": res.get("files_changed"), "warnings": res.get("warnings"),
                  "error": res.get("error")})
        if not res.get("ok"):
            e["result"] = "RED: the USA crate was not imported: %s" % res.get("error")
            return e
        mod_dir = os.path.join(mods, res["id"])
        with open(os.path.join(mod_dir, "mod.json"), encoding="utf-8") as f:
            m = json.load(f)
        v = modtool(tool, ["verify", mod_dir, "--iso", ntsc])
        new_entries, bad, _ = recomposed_archive(ndisc, mod_dir, out, {ufx["bank"].upper(): bank2})
        mh_ok = overlay.composed_sha256(ndisc, mod_dir, "CRASH6/MUSIC.MH") == hashlib.sha256(bytes(mh)).hexdigest()
        base = m.get("base") or {}
        imp = m.get("import") or {}
        e.update({"verify": v.get("ok"), "entries": len(new_entries), "entries_bad": bad, "music_mh": mh_ok, "base": base,
                  "notes": m.get("notes"), "game_code_settings": imp.get("game_code_settings")})
        ok = v.get("ok") and not bad and len(new_entries) == len(entries) and mh_ok and \
            sorted(res.get("files_changed") or []) == sorted(ufx["files_changed"]) and \
            base.get("region") == ufx["region"] and base.get("executable") == ufx["executable"] and \
            ((base.get("disc") or {}).get("redump") or "").startswith(ufx["redump_starts"]) and \
            ufx["note"] in (m.get("notes") or []) and ufx["warning"] in (res.get("warnings") or []) and \
            imp.get("game_code_settings") == ufx["settings"] and ins.get("ok") and ufx["note"] in (ins.get("notes") or []) and \
            ufx["warning"] in (ins.get("warnings") or []) and sorted(ins.get("files_changed") or []) == sorted(ufx["files_changed"])
        # over the European disc: imported, with the warning that it looks made for the North American disc
        pal_mods = os.path.join(out, "mods_pal_usa")
        r2 = modtool(tool, ["import", crate, "--iso", iso, "--mods", pal_mods])
        e["over_european_disc"] = {"ok": r2.get("ok"), "warnings": r2.get("warnings"), "files_added": r2.get("files_added")}
        ok = ok and r2.get("ok") and any(w.startswith(ufx["european_warning_starts"]) for w in (r2.get("warnings") or []))
        shutil.rmtree(pal_mods, ignore_errors=True)
        e["result"] = "GREEN" if ok else "RED: %s" % {k: e.get(k) for k in ("files_changed", "verify", "entries", "entries_bad", "music_mh", "base",
                                                                             "notes", "warnings", "game_code_settings", "inspect", "over_european_disc")}
        return e
    finally:
        shutil.rmtree(crate, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
