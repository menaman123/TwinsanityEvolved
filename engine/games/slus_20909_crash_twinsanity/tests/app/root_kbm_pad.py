#!/usr/bin/env python3
"""RootKbmPadOnly / RootPadHotplug: the input-source contract (the 2026-10-13 kbm BUG) on the actual
binary in a LIVE play session (a window, wall-paced, the live PlatformInput, the pad recorded).

CONTRACT: a connected gamepad is used when present; keyboard/mouse are ALWAYS live as a pad; the
ACTIVE source is whichever produced the last event; hot-plug both ways; select=Tab, dpad=arrows.

Arm `kbm` (RootKbmPadOnly): NO gamepad (run.meta input_sources gamepad=absent, else SKIP 77: the
machine has a pad plugged in). A scripted key sequence through the REAL SDL key route
(harness.inject_key: Space@399 and Space@1850 = cross through the title menu into the beach,
W@2100-2300 held = left stick up, Tab@2320 = select) drives the title and moves Crash:
  * the recorded padlog (what the game was served) has CROSS at the two presses, LY=0 (up) rows across
    the W hold, SELECT at the Tab -- the keyboard IS the pad, select/dpad bound by default;
  * run.meta load_windows has the level load (a window starting in [480, 620]) -- the title menu was
    driven by the keyboard;
  * the presented frame differs: frames.fhash at 2099 != at 2300 (Crash moved);
  * input_sources: kbm=on, active=kbm, gamepad=absent; finalized=1, frames = the cap.
Arm `hotplug` (RootPadHotplug): the same route to the beach, then harness.virtual_pad attaches an SDL
virtual game controller at 2000 (the real JOYDEVICEADDED route), holds its cross 2050-2080 (real
controller events), detaches it at 2150 (JOYDEVICEREMOVED), then the keyboard's W@2200-2300:
  * input_sources hotplug_added >= 1, hotplug_removed >= 1, switches >= 2 (kbm -> gamepad -> kbm),
    active=kbm, gamepad=absent at the end;
  * the recorded padlog has CROSS in [2050, 2090] (the pad's press reached the game) and LY=0 rows in
    [2200, 2300] (the keyboard took over after the unplug) -- no input lost either way;
  * stderr `nativeee: harness.virtual_pad attach/press/release/detach at boundary` x4.
Red-before (the tester's bug): with no controller connected the keyboard produced NOTHING -- no CROSS,
no load window past the title, LY stays neutral. SKIP 77 without the title files or a display.
Selftest pins the judge (no game process).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
INJECT = "nativeee: harness.inject_key "
VPAD = "nativeee: harness.virtual_pad "


def parse_meta(path):
    meta = {}
    if not os.path.isfile(path):
        return meta
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def parse_census(value):
    out = {}
    for item in (value or "").split(","):
        if "=" in item:
            k, v = item.split("=", 1)
            out[k] = v
    return out


def windows(value):
    out = []
    for item in (value or "").split(","):
        if "-" in item:
            a, b = item.split("-", 1)
            try:
                out.append((int(a), int(b)))
            except ValueError:
                pass
    return out


def parse_padlog(text):
    """[(from, to, token)] from 'A-B:TOKEN' rows."""
    rows = []
    for line in text.splitlines():
        if line.startswith("#") or ":" not in line:
            continue
        span, token = line.split(":", 1)
        a, _, b = span.partition("-")
        try:
            rows.append((int(a), int(b or a), token.strip()))
        except ValueError:
            pass
    return rows


def has_token(rows, token, lo, hi):
    return any(t == token and not (b < lo or a > hi) for a, b, t in rows)


def parse_fhash(text):
    out = {}
    for line in text.splitlines():
        if "," in line:
            k, v = line.split(",", 1)
            try:
                out[int(k)] = v.strip()
            except ValueError:
                pass
    return out


def judge_common(results, meta, padlog_rows, frames, press1, press2):
    def check(ok, what):
        results.append((bool(ok), what))
    check(meta.get("finalized") == "1", "run.meta finalized=1 (got %s)" % meta.get("finalized"))
    check(meta.get("frames") == str(frames), "run.meta frames=%d (got %s)" % (frames, meta.get("frames")))
    check(meta.get("video") == "gl", "run.meta video=gl (got %s)" % meta.get("video"))
    check(has_token(padlog_rows, "CROSS", press1, press1 + 12), "recorded padlog: CROSS at the first key press (~%d)" % press1)
    check(has_token(padlog_rows, "CROSS", press2, press2 + 12), "recorded padlog: CROSS at the second key press (~%d)" % press2)
    level = [w for w in windows(meta.get("load_windows")) if 480 <= w[0] <= 620]
    check(level, "run.meta load_windows has the level load (a window starting in [480, 620]; got %s)" % meta.get("load_windows", "<absent>"))
    src = parse_census(meta.get("input_sources"))
    check(src.get("kbm") == "on", "input_sources kbm=on (got %s)" % meta.get("input_sources", "<absent>"))
    check(src.get("active") == "kbm", "input_sources active=kbm at the end (got %s)" % src.get("active"))
    return src


def judge_kbm(meta, padlog_rows, fhash, frames, press1, press2, hold, select):
    results = []
    src = judge_common(results, meta, padlog_rows, frames, press1, press2)
    results.append((src.get("gamepad") == "absent", "input_sources gamepad=absent (got %s)" % src.get("gamepad")))
    results.append((has_token(padlog_rows, "LY=0", hold[0], hold[1]), "recorded padlog: LY=0 (left stick up) across the W hold %s" % (hold,)))
    results.append((has_token(padlog_rows, "SELECT", select, select + 12), "recorded padlog: SELECT at the Tab press (~%d; select=Tab bound by default)" % select))
    a, b = fhash.get(hold[0] - 1), fhash.get(hold[1])
    results.append((a is not None and b is not None and a != b,
                    "presented frame differs: frames.fhash %d (%s) != %d (%s)" % (hold[0] - 1, a, hold[1], b)))
    return results


def judge_hotplug(log_text, meta, padlog_rows, frames, press1, press2, pad_press, hold):
    results = []
    src = judge_common(results, meta, padlog_rows, frames, press1, press2)

    def num(key):
        try:
            return int(src.get(key, "-1"))
        except ValueError:
            return -1
    results.append((num("hotplug_added") >= 1, "input_sources hotplug_added >= 1 (got %s)" % src.get("hotplug_added")))
    results.append((num("hotplug_removed") >= 1, "input_sources hotplug_removed >= 1 (got %s)" % src.get("hotplug_removed")))
    results.append((num("switches") >= 2, "input_sources switches >= 2 (kbm -> gamepad -> kbm; got %s)" % src.get("switches")))
    results.append((src.get("gamepad") == "absent", "input_sources gamepad=absent after the detach (got %s)" % src.get("gamepad")))
    results.append((has_token(padlog_rows, "CROSS", pad_press[0], pad_press[1] + 10), "recorded padlog: CROSS from the virtual pad's press %s" % (pad_press,)))
    results.append((has_token(padlog_rows, "LY=0", hold[0], hold[1]), "recorded padlog: LY=0 across the keyboard's W hold after the unplug %s" % (hold,)))
    verbs = [l for l in log_text.splitlines() if l.startswith(VPAD)]
    results.append((len(verbs) == 4, "stderr: four `%s...` lines (attach/press/release/detach; got %d)" % (VPAD, len(verbs))))
    return results


def selftest():
    ok = True
    pad = "# padlog v1\n399-404:CROSS\n1850-1856:CROSS\n2100-2300:LY=0\n2320-2325:SELECT\n"
    rows = parse_padlog(pad)
    meta = {"finalized": "1", "frames": "2400", "video": "gl", "load_windows": "60-111,418-418,531-1174",
            "input_sources": "kbm=on,gamepad=absent,active=kbm,switches=0,hotplug_added=0,hotplug_removed=0"}
    fh = {2099: "0x1", 2300: "0x2"}
    r = judge_kbm(meta, rows, fh, 2400, 399, 1850, (2100, 2300), 2320)
    ok &= all(x for x, _ in r)
    # the tester's bug: the keyboard produced nothing
    r = judge_kbm(meta | {"load_windows": "60-111,418-418"}, parse_padlog("# padlog v1\n"), {2099: "0x1", 2300: "0x1"}, 2400, 399, 1850, (2100, 2300), 2320)
    ok &= sum(1 for x, _ in r if not x) == 6
    hp = "# padlog v1\n399-404:CROSS\n1850-1856:CROSS\n2050-2080:CROSS\n2200-2300:LY=0\n"
    hmeta = dict(meta, input_sources="kbm=on,gamepad=absent,active=kbm,switches=2,hotplug_added=1,hotplug_removed=1")
    log = "".join(VPAD + "%s at boundary %d\n" % (v, f) for v, f in (("attach", 2000), ("press", 2050), ("release", 2080), ("detach", 2150)))
    r = judge_hotplug(log, hmeta, parse_padlog(hp), 2400, 399, 1850, (2050, 2080), (2200, 2300))
    ok &= all(x for x, _ in r)
    # the pad's press lost / no switch back
    r = judge_hotplug(log, dict(hmeta, input_sources="kbm=on,gamepad=connected,active=gamepad,switches=1,hotplug_added=1,hotplug_removed=0"),
                      parse_padlog("# padlog v1\n399-404:CROSS\n1850-1856:CROSS\n"), 2400, 399, 1850, (2050, 2080), (2200, 2300))
    ok &= sum(1 for x, _ in r if not x) == 6
    print("root_kbm_pad selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def run(args, cmd, log, progress):
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "1"
    env["RB_PROGRESS_FILE"] = progress
    env["RB_STALL_S"] = str(min(args.timeout, 120))
    proc = subprocess.run([args.runner, str(args.timeout), log, "--"] + cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    return proc.returncode


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--arm", choices=("kbm", "hotplug"), default="kbm")
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=2400)
    ap.add_argument("--timeout", type=int, default=400)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    name = "RootKbmPadOnly" if args.arm == "kbm" else "RootPadHotplug"
    press1, press2, hold, select, pad_press = 399, 1850, (2100, 2300), 2320, (2050, 2080)
    # The first press skips the intro FMV. It is HELD through 3 boundaries (a real key press
    # lasts frames): an instantaneous down+up pushed in one pump is consumed by the movie's own
    # pad poll before the boundary stamp the padlog records -- the movie skipped, the recorder
    # saw no CROSS (1 of 3 runs on the 2026-10-13 boss candidate). The judge is unchanged.
    if args.arm == "kbm":
        keys = "Space@%d-%d,Space@%d,W@%d-%d,Tab@%d" % (press1, press1 + 3, press2, hold[0], hold[1], select)
        extra = []
    else:
        hold = (2200, 2300)
        keys = "Space@%d-%d,Space@%d,W@%d-%d" % (press1, press1 + 3, press2, hold[0], hold[1])
        extra = ["--set", "harness.virtual_pad=attach@2000,press@%d-%d,detach@2150" % pad_press]
    root = tempfile.mkdtemp(prefix="root_kbm_pad_%s_" % args.arm)
    out = os.path.join(root, "leg")
    os.makedirs(out)
    for d in ("mc", "logs"):
        os.makedirs(os.path.join(root, d))
    log = os.path.join(root, "leg.log")
    padlog = os.path.join(out, "session.padlog")
    code = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", out,
                      "--game-files", os.path.abspath(args.game_files), "--frames", str(args.frames),
                      "--record-input", padlog,
                      "--set", "paths.mc_dir=" + os.path.join(root, "mc"),
                      "--set", "paths.log_dir=" + os.path.join(root, "logs"),
                      "--set", "harness.inject_key=" + keys] + extra, log, os.path.join(out, "frames.fhash"))
    log_text = open(log).read() if os.path.isfile(log) else ""
    for line in log_text.splitlines():
        if line.startswith(INJECT) or line.startswith(VPAD) or line.startswith("input:"):
            print("stderr: " + line[:300])
    meta = parse_meta(os.path.join(out, "run.meta"))
    for key in ("frames", "finalized", "park", "park_detail", "video", "load_windows", "input_sources", "pad_live"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")[:200]))
    if code == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    rows = parse_padlog(open(padlog).read() if os.path.isfile(padlog) else "")
    print("padlog rows: %d; %s" % (len(rows), " ".join("%d-%d:%s" % r for r in rows[:12])))
    src = parse_census(meta.get("input_sources"))
    if args.arm == "kbm" and src.get("gamepad") == "connected":
        print("SKIP: a gamepad is connected to this machine (the kbm-only arm needs none)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    if args.arm == "kbm":
        fhash = parse_fhash(open(os.path.join(out, "frames.fhash")).read() if os.path.isfile(os.path.join(out, "frames.fhash")) else "")
        results = judge_kbm(meta, rows, fhash, args.frames, press1, press2, hold, select)
    else:
        results = judge_hotplug(log_text, meta, rows, args.frames, press1, press2, pad_press, hold)
    failures = 0
    for ok, what in results:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code != 0:
        print("  FAIL: runner exit %d (the session must run to its cap)" % code)
        failures += 1
    if failures:
        print("%s: FAIL (%d) -- leg kept at %s" % (name, failures, root))
        return 1
    print("%s: PASS (%s)" % (name, meta.get("input_sources")))
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print("kept at %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
