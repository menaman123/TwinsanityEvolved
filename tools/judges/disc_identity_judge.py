#!/usr/bin/env python3
"""disc_identity_judge.py -- does the game (and the launcher) say what a disc image IS, judged by content?

: retail = the whole image is a known retail dump (launcher/discs/redump.json); a known release's
program on an image that is no dump = a MODIFIED image; the title's files with another program = unknown but plausible;
no title files = refused; another release's program without its address map = refused by the region-aware loader. The
checks, every expectation frozen in fixtures/judges/disc_identity_pal.json:

  (1) USER   the PAL image used for development: the game's boot line and region line (--dry-run), the whole-image hashes with
             --verify-disc, tools/discs/manifest.py --identify (no dump) and --label retail (refused, nothing written)
  (2) PLAUSIBLE  a tiny ISO 9660 image the judge writes (SYSTEM.CNF booting SLES_525.68, a made-up ELF of that name, the
             core file names with byte patterns): unknown but plausible, with the warning
  (3) REFUSED   the same without the core files: refused, the files named
  (4) NTSC-U    one booting a made-up SLUS_209.09 with the core files: the region row by content and its built-in map, which
             is for the original program only -> the game stops (exit 4) with the sentence a player reads
  (5) LAUNCHER  the launcher's screens test with the real title and the development disc (PS2R_SCREENS_REPORT): the card's badge
             while the image is hashed, then the verdict, the line and the Games card -- skipped (and RED) without a launcher
             build
  (6) NTSC-U REAL  with --ntsc-iso (a retail North American image): the game's boot line by size and, with --verify-disc,
             "retail NTSC-U image SLUS-20909 v2.00 (Redump disc 13689; volume CRASH)" with the image's hashes;
             tools/discs/manifest.py --identify (RETAIL) and --check launcher/discs/ntsc_u.json (the retail file list: every
             file matches); the launcher's card on the real disc (test_launcher_models realDiscIdentity): "NTSC-U · original
             disc". SKIPPED (not RED) when --ntsc-iso is not given or the file is absent

A one-byte-modified copy of the development image is not made: the image is a modified image already (the fixture says why).
The generated images hold byte patterns only (no game data) and are deleted at the end.

Usage: disc_identity_judge.py --bin BIN --iso ISO [--ntsc-iso NTSC] [--out DIR] [--fixture F] [--launcher-build DIR] [--skip-verify]
                              [--tools-root DIR]
Writes OUT/report.json. Exit 0 = GREEN, 1 = RED, 2 = the setup failed.
"""
import argparse
import json
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "disc_identity_pal.json")
SECTOR = 2048


# ----------------------------------------------------------------------------- a tiny ISO 9660 volume (byte patterns)
def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def record(lba, size, is_dir, name):
    n = len(name)
    length = 33 + n + ((33 + n) & 1)
    r = bytearray(length)
    r[0] = length
    r[2:10] = both32(lba)
    r[10:18] = both32(size)
    r[25] = 2 if is_dir else 0
    r[28:32] = both16(1)
    r[32] = n
    r[33:33 + n] = name
    return bytes(r)


def pattern(n, seed):
    out, x = bytearray(n), (seed * 2654435761 + 1) & 0xFFFFFFFF
    for i in range(n):
        x = (x * 1103515245 + 12345) & 0xFFFFFFFF
        out[i] = (x >> 16) & 0xFF
    return bytes(out)


def fake_elf(n=6000):
    b = bytearray(pattern(n, 9))
    b[0:20] = bytes([0x7F, ord("E"), ord("L"), ord("F"), 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 2, 0, 8, 0])
    b[24:28] = struct.pack("<I", 0x00100008)
    b[28:52] = bytes(24)   # no program headers: the loader refuses it after the identity line
    return bytes(b)


def write_iso(path, volume, files):
    """files: [(dir, name, bytes)] with dir '' or 'CRASH6'. PVD 16, terminator 17, root 18, CRASH6 19, files from 20."""
    lba, placed = 20, []
    for d, name, data in files:
        placed.append((d, name, data, lba))
        lba += max(1, (len(data) + SECTOR - 1) // SECTOR)
    img = bytearray(lba * SECTOR)
    root = record(18, SECTOR, True, b"\x00") + record(18, SECTOR, True, b"\x01") + record(19, SECTOR, True, b"CRASH6")
    sub = record(19, SECTOR, True, b"\x00") + record(18, SECTOR, True, b"\x01")
    for d, name, data, at in placed:
        rec = record(at, len(data), False, (name + ";1").encode())
        if d:
            sub += rec
        else:
            root += rec
        img[at * SECTOR:at * SECTOR + len(data)] = data
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    img[19 * SECTOR:19 * SECTOR + len(sub)] = sub
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[40:72] = volume.encode().ljust(32, b" ")
    pvd[80:88] = both32(lba)
    pvd[128:132] = both16(SECTOR)
    rr = record(18, SECTOR, True, b"\x00")
    pvd[156:156 + len(rr)] = rr
    img[16 * SECTOR:17 * SECTOR] = pvd
    term = bytearray(SECTOR)
    term[0] = 0xFF
    term[1:6] = b"CD001"
    img[17 * SECTOR:18 * SECTOR] = term
    with open(path, "wb") as f:
        f.write(img)


def core_files():
    return [("CRASH6", "CRASH.BH", pattern(3000, 1)), ("CRASH6", "CRASH.BD", pattern(70000, 2)),
            ("CRASH6", "MUSIC.MH", pattern(400, 3)), ("CRASH6", "MUSIC.MB", pattern(9000, 4))]


def system_cnf(program, ver, vmode):
    text = "BOOT2 = cdrom0:\\%s;1\r\nVER = %s\r\n" % (program, ver)
    if vmode:
        text += "VMODE = %s\r\n" % vmode
    return text.encode()


# ----------------------------------------------------------------------------- runs
def run(args, env_extra=None, timeout=900):
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.update(env_extra or {})
    p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=ROOT, timeout=timeout)
    return p.returncode, p.stdout.decode("utf-8", "replace")


def lines_with(text, prefix):
    return [l for l in text.splitlines() if l.startswith(prefix)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--launcher-build", default=os.path.join(ROOT, "build-launcher"))
    ap.add_argument("--skip-verify", action="store_true", help="skip the --verify-disc run (the whole image hashed)")
    ap.add_argument("--tools-root", default=ROOT, help="the tree whose tools/discs/manifest.py is judged")
    ap.add_argument("--ntsc-iso", default="", help="a retail North American image (SLUS-20909): the real NTSC-U case (6)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or tempfile.mkdtemp(prefix="disc_identity_judge.")
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="disc_identity_judge_work.")
    report = {"judge": "disc_identity_judge", "bin": a.bin, "iso": a.iso, "checks": {}, "reasons": []}
    checks, reasons = report["checks"], report["reasons"]

    def check(name, ok, detail):
        checks[name] = {"ok": bool(ok), "detail": detail}
        if not ok:
            reasons.append("%s: %s" % (name, detail if isinstance(detail, str) else json.dumps(detail)[:400]))
        print("disc_identity_judge: %-18s %s" % (name, "GREEN" if ok else "RED"), flush=True)

    try:
        if not os.path.isfile(a.iso) or not os.access(a.bin, os.X_OK):
            print("disc_identity_judge: the disc or the binary is missing")
            return 2
        # (1) the development disc
        u = fx["user_disc"]
        rc, text = run([a.bin, "--iso", a.iso] + u["args"])
        open(os.path.join(out, "user_disc.log"), "w").write(text)
        disc = lines_with(text, "[twinsanityevolved] disc:")
        region = lines_with(text, "[twinsanityevolved] region:")
        check("user_disc", rc == u["exit"] and disc == [u["disc_line"]] and region == [u["region_line"]],
              {"exit": rc, "disc": disc, "region": region})
        if not a.skip_verify:
            v = fx["user_disc_verified"]
            rc, text = run([a.bin, "--iso", a.iso] + v["args"])
            open(os.path.join(out, "user_disc_verified.log"), "w").write(text)
            disc = lines_with(text, "[twinsanityevolved] disc:")
            check("user_disc_verified", rc == 0 and len(disc) == 1 and disc[0].startswith(v["line_starts"]), {"exit": rc, "disc": disc})
        m = fx["manifest"]
        rc, text = run([sys.executable, os.path.join(a.tools_root, "tools", "discs", "manifest.py"), a.iso, "--identify"])
        open(os.path.join(out, "manifest_identify.log"), "w").write(text)
        check("manifest_identify", rc == m["identify_exit"] and m["identify_text"] in text, {"exit": rc, "text": text.strip()[-400:]})
        listing = os.path.join(work, "should_not_exist.json")
        rc, text = run([sys.executable, os.path.join(a.tools_root, "tools", "discs", "manifest.py"), a.iso, "--id", "judge", "--label", "retail", "--out", listing])
        open(os.path.join(out, "manifest_label_retail.log"), "w").write(text)
        check("manifest_retail_no", rc == m["label_retail_exit"] and m["label_retail_text"] in text and not os.path.exists(listing),
              {"exit": rc, "written": os.path.exists(listing), "text": text.strip()[-400:]})
        # (2) unknown but plausible
        p = fx["plausible"]
        iso = os.path.join(work, "plausible.iso")
        write_iso(iso, p["volume"], [("", "SYSTEM.CNF", system_cnf("SLES_525.68", "1.01", "PAL")), ("", "SLES_525.68", fake_elf())] + core_files())
        rc, text = run([a.bin, "--iso", iso, "--config", "none", "--dry-run"])
        open(os.path.join(out, "plausible.log"), "w").write(text)
        disc = lines_with(text, "[twinsanityevolved] disc:")
        check("plausible", disc == [p["disc_line"]], {"exit": rc, "disc": disc})
        # (3) refused
        r = fx["refused"]
        iso = os.path.join(work, "refused.iso")
        write_iso(iso, r["volume"], [("", "SYSTEM.CNF", system_cnf("SLES_525.68", "1.01", "PAL")), ("", "SLES_525.68", fake_elf())])
        rc, text = run([a.bin, "--iso", iso, "--config", "none", "--dry-run"])
        open(os.path.join(out, "refused.log"), "w").write(text)
        disc = lines_with(text, "[twinsanityevolved] disc:")
        check("refused", disc == [r["disc_line"]], {"exit": rc, "disc": disc})
        # (4) the North American program: the region-aware loader's refusal
        n = fx["ntsc_u"]
        iso = os.path.join(work, "ntsc_u.iso")
        write_iso(iso, n["volume"], [("", "SYSTEM.CNF", system_cnf("SLUS_209.09", "1.00", "")), ("", "SLUS_209.09", fake_elf())] + core_files())
        rc, text = run([a.bin, "--iso", iso, "--config", "none", "--dry-run"])
        open(os.path.join(out, "ntsc_u.log"), "w").write(text)
        region = lines_with(text, "[twinsanityevolved] region:")
        refusal = lines_with(text, "[twinsanityevolved] Crash Twinsanity:")
        want_region = n["region_lines"] if "region_lines" in n else [n["region_line"]]
        check("ntsc_u_refused", rc == n["exit"] and region == want_region and refusal == [n["refusal_line"]],
              {"exit": rc, "region": region, "refusal": refusal})
        # (5) the launcher's card
        lf = fx["launcher"]
        screens = os.path.join(a.launcher_build, "test_launcher_screens")
        if not os.access(screens, os.X_OK):
            check("launcher_card", False, "no launcher build at %s (cmake -S engine/app/launcher -B build-launcher)" % a.launcher_build)
        else:
            rep = os.path.join(work, "screens_report.json")
            rc, text = run([screens], {"QT_QPA_PLATFORM": "offscreen", "PS2R_SCREENS_MANIFEST": os.path.join(ROOT, "launcher", "title.json"),
                                       "PS2R_SCREENS_ISO": a.iso, "PS2R_SCREENS_REPORT": rep}, timeout=lf["timeout_s"])
            open(os.path.join(out, "launcher_screens.log"), "w").write(text)
            got = json.load(open(rep)) if os.path.isfile(rep) else {}
            if got:
                shutil.copy(rep, os.path.join(out, "launcher_report.json"))
            keys = ["first_badge", "setup_badges", "setup_lines", "games_badge", "games_status", "disc_verdict", "disc_image_crc32", "disc_image_sha1"]
            diff = {k: {"want": lf[k], "got": got.get(k)} for k in keys if got.get(k) != lf[k]}
            check("launcher_card", rc == 0 and got and not diff, {"exit": rc, "report": bool(got), "differ": diff})
        # (6) the real North American disc
        nr = fx.get("ntsc_u_real")
        if nr and a.ntsc_iso and os.path.isfile(a.ntsc_iso):
            rc, text = run([a.bin, "--iso", a.ntsc_iso] + nr["args"])
            open(os.path.join(out, "ntsc_real.log"), "w").write(text)
            disc = lines_with(text, "[twinsanityevolved] disc:")
            check("ntsc_real_disc", disc == [nr["disc_line"]], {"exit": rc, "disc": disc})
            if not a.skip_verify:
                rc, text = run([a.bin, "--iso", a.ntsc_iso] + nr["verify_args"])
                open(os.path.join(out, "ntsc_real_verified.log"), "w").write(text)
                disc = lines_with(text, "[twinsanityevolved] disc:")
                check("ntsc_real_verified", len(disc) == 1 and disc[0].startswith(nr["verified_line_starts"]), {"exit": rc, "disc": disc})
            tool = os.path.join(a.tools_root, "tools", "discs", "manifest.py")
            rc, text = run([sys.executable, tool, a.ntsc_iso, "--identify"])
            open(os.path.join(out, "ntsc_real_identify.log"), "w").write(text)
            check("ntsc_real_identify", rc == 0 and nr["identify_text"] in text, {"exit": rc, "text": text.strip()[-300:]})
            listing = os.path.join(a.tools_root, nr["file_list"])
            rc, text = run([sys.executable, tool, a.ntsc_iso, "--check", listing])
            open(os.path.join(out, "ntsc_real_check.log"), "w").write(text)
            doc = json.load(open(listing)) if os.path.isfile(listing) else {}
            check("ntsc_real_file_list", rc == 0 and nr["check_text"] in text and doc.get("retail") is True and
                  doc.get("image", {}).get("redump", {}).get("match") == nr["redump_id"], {"exit": rc, "text": text.strip()[-300:],
                  "retail": doc.get("retail"), "match": doc.get("image", {}).get("redump", {}).get("match")})
            models = os.path.join(a.launcher_build, "test_launcher_models")
            if not os.access(models, os.X_OK):
                check("ntsc_real_launcher", False, "no launcher build at %s (cmake -S engine/app/launcher -B build-launcher)" % a.launcher_build)
            else:
                rep = os.path.join(work, "identity_report.json")
                rc, text = run([models, "realDiscIdentity"], {"QT_QPA_PLATFORM": "offscreen", "PS2R_IDENTITY_ISO": a.ntsc_iso,
                                                                "PS2R_IDENTITY_TITLE": os.path.join(ROOT, "launcher", "title.json"),
                                                                "PS2R_IDENTITY_REPORT": rep, "PS2R_IDENTITY_BADGE": nr["launcher"]["badge"]},
                               timeout=nr["launcher"]["timeout_s"])
                open(os.path.join(out, "ntsc_real_launcher.log"), "w").write(text)
                got = json.load(open(rep)) if os.path.isfile(rep) else {}
                if got:
                    shutil.copy(rep, os.path.join(out, "ntsc_real_launcher_report.json"))
                diff = {k: {"want": v, "got": got.get(k)} for k, v in nr["launcher"].items() if k not in ("timeout_s", "why") and got.get(k) != v}
                check("ntsc_real_launcher", rc == 0 and got and not diff, {"exit": rc, "report": bool(got), "differ": diff})
        elif nr:
            checks["ntsc_real"] = {"ok": True, "skipped": True,
                                   "detail": "SKIPPED: no --ntsc-iso" if not a.ntsc_iso else "SKIPPED: %s is absent" % a.ntsc_iso}
            print("disc_identity_judge: %-18s SKIPPED (%s)" % ("ntsc_real", checks["ntsc_real"]["detail"]), flush=True)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report["verdict"] = "GREEN" if not reasons else "RED"
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2, ensure_ascii=False)
    print("disc_identity_judge: %s -- %d checks%s -> %s" % (report["verdict"], len(checks),
          "" if not reasons else "; " + "; ".join(r[:160] for r in reasons), os.path.join(out, "report.json")))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
