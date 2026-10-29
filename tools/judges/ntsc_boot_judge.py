#!/usr/bin/env python3
"""ntsc_boot_judge.py -- does the North American disc play, the European game's code running on it through the map?

The port runs the decompilation's C++ (the European program's); the North American disc (SLUS-20909) is played through its
address map, built into the game (launcher/discs/region_map_SLUS-20909.json). With --ntsc-iso (the retail
NTSC-U image) and the European disc (--iso, for the map's executable), every expectation frozen in
fixtures/judges/ntsc_boot_ntsc.json:

  (1) IDENTITY  tools/discs/manifest.py --identify: the image is the retail dump the map was made for (Redump 13689, v2.00)
  (2) MAP       the built-in map was made from THIS disc's program (its sha256) against the European one (the PAL disc's)
  (3) DRY RUN   the game's boot lines: the region row through the built-in map, the data image laid out (the counts), the
                archive entry renamed for the European program's font, the code-pointer patch's counts, ONE language row
                ('english', the voice Crash6\\American.mb), and game.hz = 50 refused with its sentence (the disc runs at 60)
  (4) TITLE     deterministic, headless, --skip-movies to 2600: the state trace reaches Title (7) by the fixture's boundary and
                the menus draw text (the game's font)
  (5) HUB       the hub pad log timed for this disc (fixtures/padlogs/hub_entry_ntsc.padlog) to 4200: the route's states in
                order, Playing held at the end, no trap, the field rate 60 (run.meta field_hz, the clock census), the heap's
                small pages left (padlog_judge's checks, fixtures/judges/hub_entry_ntsc.json)
  (6) RATE      the same pad log in PLAY mode (paced by the clock, headless; every tick logged, every line stamped): the game's
                rate over the Playing span, boundaries per wall second = 60.0 +- the fixture's tolerance; no catch-up, no
                dropped field
  (7) MOVIES    the boot WITHOUT --skip-movies (deterministic, headless): the Vivendi and Travellers Tales logos of this disc
                play through the game's Player (tools/judges/movie_judge.py's census checks, the fixture's thresholds)

Without --ntsc-iso the judge writes a report saying so and exits 2 (SKIPPED). A binary that refuses the disc is RED.

Usage: ntsc_boot_judge.py --bin BIN --iso PAL_ISO [--ntsc-iso NTSC_ISO] [--out DIR] [--fixture F] [--only CHECK,...]
Writes OUT/report.json. Exit 0 = GREEN, 1 = RED, 2 = SKIPPED (no NTSC-U image), 3 = the setup failed (a disc or the binary missing).
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "ntsc_boot_ntsc.json")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")
STAMPED_STATE = re.compile(r"^(\d+\.\d+) \[twinsanityevolved\] game state @ boundary (\d+): (\w+) \((\d+)\)")
STAMPED_TICK = re.compile(r"^(\d+\.\d+) \[twinsanityevolved\] tick: boundary (\d+) took ([\d.]+) ms")
CLOCK = re.compile(r"clock: (\d+) boundaries, (\d+) fields, (\d+) parks, (\d+) late, (\d+) timeouts \((\w+) (\d+) Hz\); catch-up (\d+) boundaries, (\d+) fields dropped")
TEXT_QUADS = re.compile(r"\[te\.gfx\] summary: .*?text quads (\d+)")


def run(args, env_extra=None, timeout=1200, log=None, stamped=False):
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.update(env_extra or {})
    try:
        if stamped:   # every stderr line stamped with time.monotonic() (the game's rate is read from the tick lines)
            lines = []
            p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, cwd=ROOT, text=True, errors="replace")
            deadline = time.monotonic() + timeout
            for line in p.stderr:
                lines.append("%.3f %s" % (time.monotonic(), line))
                if time.monotonic() > deadline:
                    p.kill()
                    break
            rc = p.wait()
            text = "".join(lines)
        else:
            p = subprocess.run(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=ROOT, timeout=timeout)
            rc, text = p.returncode, p.stdout.decode("utf-8", "replace")
    except (OSError, subprocess.TimeoutExpired) as e:
        rc, text = -1, "could not run %s: %s" % (args[0], e)
    if log:
        with open(log, "w") as f:
            f.write(" ".join(args) + "\n" + text)
    return rc, text


def read_meta(path):
    meta = {}
    if os.path.isfile(path):
        for line in open(path, errors="replace"):
            if "=" in line:
                k, v = line.rstrip("\n").split("=", 1)
                meta[k] = v
    return meta


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True, help="the European disc (the map was made against its executable)")
    ap.add_argument("--ntsc-iso", default="")
    ap.add_argument("--out", default="")
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--only", default="", help="a comma list of checks to run (the others are not run: the report says so)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    out = a.out or tempfile.mkdtemp(prefix="ntsc_boot_judge.")
    os.makedirs(out, exist_ok=True)
    report = {"judge": "ntsc_boot_judge", "bin": a.bin, "iso": a.iso, "ntsc_iso": a.ntsc_iso, "checks": {}, "reasons": []}
    if not a.ntsc_iso or not os.path.isfile(a.ntsc_iso):
        report["verdict"] = "SKIPPED"
        report["why"] = ("no NTSC-U image: pass --ntsc-iso <the North American disc (SLUS-20909 v2.00)> to judge the European code "
                         "playing it" + ("" if not a.ntsc_iso else " (%s is absent)" % a.ntsc_iso))
        with open(os.path.join(out, "report.json"), "w") as f:
            json.dump(report, f, indent=2)
        print("ntsc_boot_judge: SKIPPED -- %s -> %s" % (report["why"], os.path.join(out, "report.json")))
        return 2
    if not os.path.isfile(a.iso) or not os.access(a.bin, os.X_OK):
        print("ntsc_boot_judge: the European disc or the binary is missing")
        return 3
    checks, reasons = report["checks"], report["reasons"]
    only = set(x for x in a.only.split(",") if x)

    def wanted(name):
        if only and name not in only:
            checks[name] = {"ok": True, "skipped": True, "detail": "not run (--only)"}
            return False
        return True

    def check(name, ok, detail):
        checks[name] = {"ok": bool(ok), "detail": detail}
        if not ok:
            reasons.append("%s: %s" % (name, detail if isinstance(detail, str) else json.dumps(detail)[:400]))
        print("ntsc_boot_judge: %-8s %s" % (name, "GREEN" if ok else "RED"), flush=True)

    py = sys.executable
    base_args = [a.bin, "--iso", a.ntsc_iso]
    # (1) the disc
    if wanted("identity"):
        rc, text = run([py, os.path.join(ROOT, "tools", "discs", "manifest.py"), a.ntsc_iso, "--identify"], log=os.path.join(out, "identify.log"))
        m = re.search(r"Redump disc (\d+)", text)
        check("identity", rc == 0 and "RETAIL" in text and m and int(m.group(1)) in fx["retail_dumps"], {"exit": rc, "text": text.strip()[-300:]})
    # (2) the built-in map is this disc's program's
    if wanted("map"):
        fm = fx["map"]
        sys.path.insert(0, os.path.join(ROOT, "tools", "discs"))
        import hashlib
        import elf_region_map as erm
        _exe, _serial, ntsc_elf = erm.elf_from_iso(a.ntsc_iso)
        _pexe, _pserial, pal_elf = erm.elf_from_iso(a.iso)
        doc = json.load(open(os.path.join(ROOT, fm["committed"])))
        got = {"map_elf": doc.get("elf", {}).get("sha256"), "disc_elf": hashlib.sha256(ntsc_elf).hexdigest(),
               "map_pal_elf": doc.get("pal_elf", {}).get("sha256"), "pal_elf": hashlib.sha256(pal_elf).hexdigest(),
               "serial": doc.get("serial"), "schema": doc.get("schema")}
        check("map", got["map_elf"] == got["disc_elf"] and got["map_pal_elf"] == got["pal_elf"] and got["serial"] == fx["serial"] and
              got["schema"] == fm["schema"], got)
    # (3) the dry run
    if wanted("dry_run"):
        fd = fx["dry_run"]
        rc, text = run(base_args + ["--config", "none", "--dry-run"], log=os.path.join(out, "dry_run.log"))
        lines = [l.rstrip() for l in text.splitlines()]
        missing = [w for w in fd["lines"] if w not in lines]
        ini = os.path.join(out, "ps2r_hz50.ini")
        with open(ini, "w") as f:
            f.write("game.hz = 50\n")
        rc2, text2 = run(base_args + ["--config", ini, "--mc-dir", os.path.join(out, "mc_dry"), "--dry-run"], log=os.path.join(out, "dry_run_hz50.log"))
        rate = fd["rate_line"] in [l.rstrip() for l in text2.splitlines()]
        check("dry_run", rc == 0 and not missing and rc2 == 0 and rate, {"exit": [rc, rc2], "missing": missing, "rate_line": rate})
    # (4) the title
    if wanted("title"):
        ft = fx["title"]
        mc = os.path.join(out, "mc_title")
        shutil.rmtree(mc, ignore_errors=True)
        os.makedirs(mc)
        rc, text = run(base_args + ["--config", "none", "--mc-dir", mc, "--skip-movies", "--deterministic", "--no-sdl-input", "--no-audio",
                                    "--trace-state", "--stop-at", str(ft["stop_at"]), "--harness-out", os.path.join(out, "title")],
                       timeout=ft["run_seconds"], log=os.path.join(out, "title.log"))
        states = [(int(b), name) for b, name, _ in STATE.findall(text)]
        title_at = next((b for b, name in states if name == "Title"), None)
        quads = [int(q) for q in TEXT_QUADS.findall(text)]
        got = {"exit": rc, "title_at": title_at, "text_quads": quads[-1] if quads else None, "trap": "wasm trap" in text}
        check("title", rc == 0 and not got["trap"] and title_at is not None and title_at <= ft["title_by"] and
              (got["text_quads"] or 0) >= ft["min_text_quads"], got)
    # (5) the hub pad log
    if wanted("hub"):
        fh = fx["hub"]
        mc = os.path.join(out, "mc_hub")
        shutil.rmtree(mc, ignore_errors=True)
        os.makedirs(mc)
        log = os.path.join(out, "hub.log")
        rc, text = run(base_args + ["--config", "none", "--mc-dir", mc, "--skip-movies", "--deterministic", "--no-sdl-input", "--no-audio",
                                    "--trace-state", "--play-pad", os.path.join(ROOT, fh["padlog"]), "--stop-at", str(fh["stop_at"]),
                                    "--harness-out", os.path.join(out, "hub")], timeout=fh["run_seconds"], log=log)
        prc, ptext = run([py, os.path.join(HERE, "padlog_judge.py"), log, "--fixture", os.path.join(ROOT, fh["padlog_fixture"])],
                         log=os.path.join(out, "hub_padlog_judge.log"))
        meta = read_meta(os.path.join(out, "hub", "run.meta"))
        c = CLOCK.search(text)
        got = {"exit": rc, "padlog_judge": prc, "padlog_judge_text": ptext.strip()[-600:], "field_hz": meta.get("field_hz"),
               "clock": c.group(0) if c else None, "trap": "wasm trap" in text, "state_trace": meta.get("state_trace")}
        check("hub", rc == 0 and prc == 0 and not got["trap"] and meta.get("field_hz") == str(fh["field_hz"]) and c and
              c.group(6) == "deterministic" and int(c.group(7)) == fh["field_hz"], got)
    # (6) the game's rate in play mode
    if wanted("rate"):
        fr = fx["rate"]
        mc = os.path.join(out, "mc_rate")
        shutil.rmtree(mc, ignore_errors=True)
        os.makedirs(mc)
        rc, text = run(base_args + ["--config", "none", "--mc-dir", mc, "--skip-movies", "--no-sdl-input", "--no-audio", "--trace-state",
                                    "--play-pad", os.path.join(ROOT, fr["padlog"]), "--stop-at", str(fr["stop_at"]),
                                    "--run-seconds", str(fr["run_seconds"])],
                       {"TE_TICK_LOG_MS": "0"}, timeout=fr["run_seconds"] + 60, log=os.path.join(out, "rate.log"), stamped=True)
        t_play = None
        for line in text.splitlines():
            s = STAMPED_STATE.match(line)
            if s and s.group(3) == "Playing" and int(s.group(2)) >= fr["playing_from"]:
                t_play = float(s.group(1))
                break
        span = []
        for line in text.splitlines():
            t = STAMPED_TICK.match(line)
            if t and t_play is not None and float(t.group(1)) >= t_play:
                span.append((float(t.group(1)), int(t.group(2))))
        game_hz = None
        if len(span) >= 60 and span[-1][0] > span[0][0]:
            game_hz = round((span[-1][1] - span[0][1]) / (span[-1][0] - span[0][0]), 2)
        c = CLOCK.search(text)
        got = {"exit": rc, "game_hz": game_hz, "span_boundaries": (span[-1][1] - span[0][1]) if span else 0,
               "span_seconds": round(span[-1][0] - span[0][0], 2) if span else 0, "clock": c.group(0) if c else None,
               "trap": "wasm trap" in text}
        check("rate", rc == 0 and not got["trap"] and game_hz is not None and abs(game_hz - fr["game_hz"]) <= fr["game_hz_tolerance"] and c and
              c.group(6) == "play" and int(c.group(8)) == 0 and int(c.group(9)) == 0, got)
    # (7) the boot movies
    if wanted("movies"):
        fv = fx["movies"]
        mc = os.path.join(out, "mc_movies")
        shutil.rmtree(mc, ignore_errors=True)
        os.makedirs(mc)
        log = os.path.join(out, "movies.log")
        rc, text = run(base_args + ["--config", "none", "--mc-dir", mc, "--deterministic", "--no-sdl-input", "--trace-state", "--stop-at",
                                    str(fv["stop_at"])], timeout=fv["run_seconds"], log=log)
        th = fv["thresholds"]
        mrc, mtext = run([py, os.path.join(HERE, "movie_judge.py"), log, "--min-opens", str(th["min_opens"]), "--min-pictures",
                          str(th["min_pictures"]), "--min-pcm", str(th["min_pcm"])], log=os.path.join(out, "movie_judge.log"))
        states = [(int(b), name) for b, name, _ in STATE.findall(text)]
        got = {"exit": rc, "movie_judge": mrc, "movie_judge_text": mtext.strip()[-500:], "title_at": next((b for b, n in states if n == "Title"), None),
               "trap": "wasm trap" in text}
        check("movies", rc == 0 and mrc == 0 and not got["trap"] and got["title_at"] is not None and got["title_at"] <= fv["title_by"], got)
    # the memory cards hold files the game copies from the disc (its icon): not kept
    for d in ("mc_dry", "mc_title", "mc_hub", "mc_rate", "mc_movies"):
        shutil.rmtree(os.path.join(out, d), ignore_errors=True)
    report["verdict"] = "GREEN" if not reasons else "RED"
    with open(os.path.join(out, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print("ntsc_boot_judge: %s -- %d checks%s -> %s" % (report["verdict"], len(checks),
          "" if not reasons else "; " + "; ".join(r[:160] for r in reasons), os.path.join(out, "report.json")))
    return 0 if not reasons else 1


if __name__ == "__main__":
    sys.exit(main())
