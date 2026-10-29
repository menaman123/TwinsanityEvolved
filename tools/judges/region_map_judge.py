#!/usr/bin/env python3
"""region_map_judge.py -- does the region-aware loader's mechanism work, and is the committed NTSC-U map the real one?

The port runs the decompilation's C++ at the PAL executable's addresses; another release's executable needs an address
map. With the PAL disc used for development (fixtures/judges/region_map_pal.json):

  (1) SELFTEST  tools/discs/elf_region_map.py selftest (synthetic executables)
  (2) IDENTITY  the map of the PAL executable against itself: one segment per data section, nothing to translate, every
                PAL data symbol mapped; the image rebuilt through it equals the PAL one
  (3) SHIFTED   a synthetic executable made from the PAL one (every section and the segment +0x10000, every pointer word
                rebased, the code's lui of the program's addresses rebased): the tool recovers +0x10000 for every segment, bss
                range and code pair, maps every symbol, and the image rebuilt from the moved executable equals the PAL one
  (4) LOADER    the game's own loader (platform/nativeee/tests/test_region_map: engine/app/host/disc_regions.cpp) over the
                same maps (and the North American one with --ntsc-iso): the rebuilt image differs in exactly the bytes the
                map lists, and the C++ map maker (the mod tool's region-map) writes the same file byte for byte
  (5) BOOT      the game RUNS from the moved executable (a temporary mod's disc/SLES_525.68 + --region-map): the hub leg to
                4200 is byte-identical (frames, audio, states, present descriptors) to the same binary's run of the PAL one
  (6) NTSC ROW  a generated disc whose SYSTEM.CNF boots SLUS_209.09 -- the moved executable under that name, the core
                files as byte patterns -- with a map made for it under SLUS-20909 and a ps2r.ini saying game.hz = 50: the
                North American row is chosen by content, its map found by the executable's hash, game.hz = 50 not applied
                (60), the data image laid out through the map and the code-pointer patch counting what it counts for PAL
                (a dry run: the generated disc has no game to play)
  (7) REAL PAIR with --ntsc-iso (the retail North American disc): the map both tools make from the two discs IS the committed
                launcher/discs/region_map_SLUS-20909.json (byte for byte), it rebuilds the image with exactly the bytes it
                lists, and its census lies within the fixture's bounds. SKIPPED (not RED) without --ntsc-iso.

The executables are read from the discs into a temporary folder and deleted at the end.

Usage: region_map_judge.py --bin BIN --iso ISO [--ntsc-iso NTSC] [--out DIR] [--fixture F] [--test-bin FILE] [--modtool FILE]
                           [--tools-root DIR] [--skip-boot]
Writes OUT/report.json. Exit 0 = GREEN, 1 = RED, 2 = the setup failed.
"""
import argparse
import json
import os
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "region_map_pal.json")


def run(args, env_extra=None, timeout=1200, log=None):
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.update(env_extra or {})
    try:
        p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=ROOT, timeout=timeout)
        rc, text = p.returncode, p.stdout.decode("utf-8", "replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        rc, text = -1, "could not run %s: %s" % (args[0], e)
    if log:
        with open(log, "w") as f:
            f.write(" ".join(args) + "\n" + text)
    return rc, text


def u32(v):
    return int(v, 16) if isinstance(v, str) else int(v)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default="")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--test-bin", default="", help="test_region_map (default: beside BIN in platform/nativeee/tests)")
    ap.add_argument("--tools-root", default=ROOT, help="the tree whose tools/discs/elf_region_map.py is judged")
    ap.add_argument("--skip-boot", action="store_true")
    ap.add_argument("--ntsc-iso", default="", help="the retail North American disc: the real pair (7)")
    ap.add_argument("--modtool", default="", help="twinsanityevolved-modtool (default: beside BIN)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or tempfile.mkdtemp(prefix="region_map_judge.")
    os.makedirs(out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="region_map_judge_work.")
    tool = os.path.join(a.tools_root, "tools", "discs", "elf_region_map.py")
    test_bin = a.test_bin or os.path.join(os.path.dirname(os.path.abspath(a.bin)), "platform", "nativeee", "tests", "test_region_map")
    shift = u32(fx["shift"])
    report = {"judge": "region_map_judge", "bin": a.bin, "iso": a.iso, "tool": tool, "checks": {}, "reasons": []}
    checks, reasons = report["checks"], report["reasons"]

    def check(name, ok, detail):
        checks[name] = {"ok": bool(ok), "detail": detail}
        if not ok:
            reasons.append("%s: %s" % (name, detail if isinstance(detail, str) else json.dumps(detail)[:300]))
        print("region_map_judge: %-9s %s" % (name, "GREEN" if ok else "RED"), flush=True)

    if not os.path.isfile(a.iso) or not os.access(a.bin, os.X_OK):
        print("region_map_judge: the disc or the binary is missing")
        return 2
    py = sys.executable
    try:
        if not os.path.isfile(tool):
            for name in ("selftest", "identity", "shifted", "loader", "boot"):
                check(name, False, "no %s in the tree judged" % os.path.relpath(tool, a.tools_root))
        else:
            rc, text = run([py, tool, "selftest"], log=os.path.join(out, "selftest.log"))
            check("selftest", rc == 0 and "PASS" in text, text.strip()[-300:])
            # (2) identity
            ident = os.path.join(work, "identity.json")
            rc, text = run([py, tool, "map", "--pal-iso", a.iso, "--other-iso", a.iso, "--serial", "SLES-52568", "--out", ident],
                           log=os.path.join(out, "identity_map.log"))
            doc = json.load(open(ident)) if rc == 0 and os.path.isfile(ident) else {}
            c = doc.get("coverage", {})
            rcv, vtext = run([py, tool, "verify", "--map", ident, "--pal-iso", a.iso, "--other-iso", a.iso], log=os.path.join(out, "identity_verify.log"))
            fi = fx["identity"]
            got = {"segments": len(doc.get("segments", [])), "aligned_bytes": c.get("aligned_bytes"), "pointer_words": c.get("pointer_words"),
                   "code_pairs": c.get("code_pairs"), "code_conflicts": c.get("code_conflicts"),
                   "symbols": [c.get("pal_data_symbols_mapped"), c.get("pal_data_symbols")],
                   "deltas": sorted({u32(s["other"]) - u32(s["pal"]) for s in doc.get("segments", []) + doc.get("bss", [])}), "verify": vtext.strip()[-200:]}
            check("identity", doc and got["segments"] == fi["segments"] and got["aligned_bytes"] == fi["aligned_bytes"] and
                  got["pointer_words"] == fi["pointer_words"] and (got["code_pairs"] or 0) >= fi["code_pairs_min"] and
                  got["code_conflicts"] == fi["code_conflicts"] and got["symbols"][0] == got["symbols"][1] and got["deltas"] == [0] and
                  rcv == 0 and (" %d of " % fi["rebuilt_differing_bytes"]) in vtext, got)
            # (3) the synthetic relocation
            moved = os.path.join(work, "shifted.elf")
            smap = os.path.join(work, "shifted_map.json")
            rc1, _ = run([py, tool, "shift", "--pal-iso", a.iso, "--shift", fx["shift"], "--out", moved], log=os.path.join(out, "shift.log"))
            rc2, _ = run([py, tool, "map", "--pal-iso", a.iso, "--other-elf", moved, "--serial", "SLES-52568", "--out", smap],
                         log=os.path.join(out, "shifted_map.log"))
            doc = json.load(open(smap)) if rc1 == 0 and rc2 == 0 and os.path.isfile(smap) else {}
            c = doc.get("coverage", {})
            rcv, vtext = run([py, tool, "verify", "--map", smap, "--pal-iso", a.iso, "--other-elf", moved], log=os.path.join(out, "shifted_verify.log"))
            fs_ = fx["shifted"]
            got = {"segment_deltas": sorted({u32(s["other"]) - u32(s["pal"]) for s in doc.get("segments", [])}),
                   "bss_deltas": sorted({u32(s["other"]) - u32(s["pal"]) for s in doc.get("bss", [])}),
                   "code_deltas": sorted({u32(o) - u32(p) for o, p in doc.get("code", [])}),
                   "pointer_words": c.get("pointer_words"), "code_conflicts": c.get("code_conflicts"),
                   "symbols": [c.get("pal_data_symbols_mapped"), c.get("pal_data_symbols")], "verify": vtext.strip()[-260:]}
            check("shifted", doc and got["segment_deltas"] == [u32(fs_["segment_delta"])] and got["bss_deltas"] == [u32(fs_["bss_delta"])] and
                  got["code_deltas"] == [shift] and (got["pointer_words"] or 0) >= fs_["pointer_words_min"] and got["code_conflicts"] == 0 and
                  got["symbols"][0] == got["symbols"][1] and rcv == 0 and (" %d of " % fs_["rebuilt_differing_bytes"]) in vtext and
                  "%d unmapped" % fs_["unmapped"] in vtext, got)
            # the North American pair (with --ntsc-iso): the executable beside the others, the Python tool's map of it
            have_ntsc = bool(a.ntsc_iso) and os.path.isfile(a.ntsc_iso)
            fr = fx["real_pair"]
            ntsc_map = os.path.join(work, "ntsc_map.json")
            if have_ntsc:
                sys.path.insert(0, os.path.join(a.tools_root, "tools", "discs"))
                import elf_region_map as erm
                _exe, _serial, data = erm.elf_from_iso(a.ntsc_iso)
                with open(os.path.join(work, "ntsc.elf"), "wb") as f:
                    f.write(data)
                rc_py, py_text = run([py, tool, "map", "--pal-iso", a.iso, "--other-iso", a.ntsc_iso, "--out", ntsc_map],
                                     timeout=fr["timeout_s"], log=os.path.join(out, "real_pair_python_map.log"))
            # (4) the game's own loader over the same maps
            fl = fx["loader_test"]
            if not os.access(test_bin, os.X_OK):
                check("loader", False, "no %s" % test_bin)
            else:
                rc, text = run([test_bin], {"TE_TEST_ISO": a.iso, "TE_TEST_REGION_MAP_DIR": work}, log=os.path.join(out, "test_region_map.log"))
                lines = [l.strip() for l in text.splitlines()]
                want = [fl["identity_line"], fl["shifted_line"], fl["identity_cpp_line"], fl["shifted_cpp_line"]]
                if have_ntsc:
                    want += [fl["ntsc_line"], fl["ntsc_cpp_line"]]
                missing = [w for w in want if w not in lines]
                check("loader", rc == 0 and not missing and "0 failures" in text, {"missing": missing, "tail": text.strip()[-600:]})
            # (5) the game from the moved executable
            fb = fx["boot"]
            if a.skip_boot:
                checks["boot"] = {"ok": True, "detail": "skipped (--skip-boot)"}
            elif not doc:
                check("boot", False, "no map of the moved executable to boot with")
            else:
                legs = {}
                for tag in ("pal", "moved"):
                    d = os.path.join(work, tag)
                    os.makedirs(os.path.join(d, "mc"))
                    args = [a.bin, "--iso", a.iso, "--config", "none", "--mc-dir", os.path.join(d, "mc"), "--skip-movies", "--deterministic",
                            "--no-sdl-input", "--no-audio", "--trace-state", "--play-pad", os.path.join(ROOT, fb["padlog"]), "--stop-at",
                            str(fb["stop_at"]), "--run-seconds", str(fb["run_seconds"]), "--harness-out", os.path.join(d, "harness")]
                    if tag == "moved":
                        mod = os.path.join(d, "mods", fb["mod_id"])
                        os.makedirs(os.path.join(mod, "disc"))
                        shutil.copy(moved, os.path.join(mod, "disc", "SLES_525.68"))
                        with open(os.path.join(mod, "mod.json"), "w") as f:
                            json.dump({"schema": 2, "id": fb["mod_id"], "kind": "disc", "name": "Relocated executable (judge)",
                                       "author": "the region map judge", "version": "1",
                                       "description": "SLES_525.68 with every section moved by %s, loaded through its address map" % fx["shift"],
                                       "source": {"kind": "folder", "artefact": fb["mod_id"], "sha256": ""},
                                       "base": {"region": "PAL", "executable": "SLES_525.68"},
                                       "files": [{"path": "SLES_525.68", "form": "replace", "bytes": os.path.getsize(moved)}]}, f, indent=2)
                        args += ["--mods-dir", os.path.join(d, "mods"), "--mod", fb["mod_id"], "--region-map", smap]
                    rc, text = run(args, timeout=fb["run_seconds"] + 120, log=os.path.join(out, "boot_%s.log" % tag))
                    legs[tag] = {"rc": rc, "text": text, "harness": os.path.join(d, "harness")}
                same = {}
                for f in fb["files"]:
                    pa, pb = os.path.join(legs["pal"]["harness"], f), os.path.join(legs["moved"]["harness"], f)
                    same[f] = os.path.isfile(pa) and os.path.isfile(pb) and open(pa, "rb").read() == open(pb, "rb").read()
                mt = legs["moved"]["text"]
                got = {"rc": [legs["pal"]["rc"], legs["moved"]["rc"]], "identical": same,
                       "loader_line": next((l for l in mt.splitlines() if l.startswith(fb["loader_line"])), ""),
                       "patch_line_same": fb["patch_line"] in mt and fb["patch_line"] in legs["pal"]["text"]}
                check("boot", got["rc"] == [0, 0] and all(same.values()) and got["loader_line"] and got["patch_line_same"], got)
            # (6) the North American row with a map (a generated disc carrying the moved executable as SLUS_209.09)
            fn = fx["ntsc_row"]
            if not doc or not os.path.isfile(moved):
                check("ntsc_row", False, "no moved executable to carry")
            else:
                sys.path.insert(0, HERE)
                import disc_identity_judge as dij
                us_iso = os.path.join(work, "ntsc_row.iso")
                exe = open(moved, "rb").read()
                dij.write_iso(us_iso, "JUDGEUSA", [("", "SYSTEM.CNF", dij.system_cnf("SLUS_209.09", "1.00", "")), ("", "SLUS_209.09", exe)] + dij.core_files())
                us_map = os.path.join(work, "region_map_SLUS-20909.json")
                rc1, _ = run([py, tool, "map", "--pal-iso", a.iso, "--other-elf", moved, "--serial", "SLUS-20909", "--out", us_map],
                             log=os.path.join(out, "ntsc_row_map.log"))
                ini = os.path.join(work, "ps2r.ini")
                with open(ini, "w") as f:
                    f.write("game.hz = 50\n")
                rc, text = run([a.bin, "--iso", us_iso, "--config", ini, "--mc-dir", os.path.join(work, "us_mc"), "--region-map", us_map, "--dry-run"],
                               log=os.path.join(out, "ntsc_row_dry_run.log"))
                lines = text.splitlines()
                got = {"exit": rc, "map_exit": rc1,
                       "region": next((l for l in lines if l.startswith("[twinsanityevolved] region: NTSC-U")), ""),
                       "rate": next((l for l in lines if "game.hz = 50" in l), ""),
                       "loader": next((l for l in lines if l.startswith(fn["loader_line_starts"])), ""),
                       "patch": fn["patch_line"] in text}
                check("ntsc_row", rc == 0 and rc1 == 0 and got["region"].startswith(fn["region_line_starts"]) and got["rate"] == fn["rate_line"] and
                      got["loader"] and got["loader"].endswith(fn["loader_line_ends"]) and got["patch"], got)
            # (7) the real pair: the committed map is the one the two discs give
            committed = os.path.join(a.tools_root, fr["committed"])
            if not have_ntsc:
                checks["real_pair"] = {"ok": True, "skipped": True,
                                       "detail": "SKIPPED: no --ntsc-iso" if not a.ntsc_iso else "SKIPPED: %s is absent" % a.ntsc_iso}
                print("region_map_judge: %-9s SKIPPED (%s)" % ("real_pair", checks["real_pair"]["detail"]), flush=True)
            else:
                modtool = a.modtool or os.path.join(os.path.dirname(os.path.abspath(a.bin)), "twinsanityevolved-modtool")
                cpp_map = os.path.join(work, "ntsc_map_cpp.json")
                rc_cpp, cpp_text = run([modtool, "region-map", "--pal", a.iso, "--disc", a.ntsc_iso, "--out", cpp_map], timeout=fr["timeout_s"],
                                       log=os.path.join(out, "real_pair_modtool.log")) if os.access(modtool, os.X_OK) else (-1, "no " + modtool)
                same = {}
                for tag, path in (("python", ntsc_map), ("modtool", cpp_map)):
                    same[tag] = os.path.isfile(path) and os.path.isfile(committed) and open(path, "rb").read() == open(committed, "rb").read()
                rcv, vtext = run([py, tool, "verify", "--map", committed, "--pal-iso", a.iso, "--other-iso", a.ntsc_iso],
                                 log=os.path.join(out, "real_pair_verify.log"))
                doc = json.load(open(committed)) if os.path.isfile(committed) else {}
                c = doc.get("coverage", {})
                b = fr["bounds"]
                got = {"python_rc": rc_py, "modtool_rc": rc_cpp, "same_as_committed": same, "verify_rc": rcv, "verify": vtext.strip()[-300:],
                       "segments": len(doc.get("segments", [])), "coverage": {k: c.get(k) for k in b}}
                inside = all(c.get(k) is not None and (v[0] is None or c.get(k) >= v[0]) and (v[1] is None or c.get(k) <= v[1]) for k, v in b.items())
                check("real_pair", rc_py == 0 and rc_cpp == 0 and all(same.values()) and rcv == 0 and inside and
                      (" %d of " % c.get("differing_bytes", -1)) in vtext and "0 unmapped" in vtext and
                      doc.get("names") == fr["names"], got)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    report["verdict"] = "GREEN" if not reasons else "RED"
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print("region_map_judge: %s -- %d checks%s -> %s" % (report["verdict"], len(checks),
          "" if not reasons else "; " + "; ".join(r[:160] for r in reasons), os.path.join(out, "report.json")))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
