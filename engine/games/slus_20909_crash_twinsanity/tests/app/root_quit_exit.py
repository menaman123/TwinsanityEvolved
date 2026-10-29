#!/usr/bin/env python3
"""RootQuitGame / RootExitClean: the pause menu's QUIT GAME and the window close, on the actual binary
in a PLAY session (a window

QUIT GAME's native meaning (TR gamecontroller.cpp:953 ActionItem::ActionTitle -> QuitToTitle ->
StateRestartingTitle -> LoadingTitle: StopChunks, loading picture, oleg.Reset, UnloadEverything +
QueueChunk(startChunk) -> StateTitle -> the title's intro movie) is RETURN TO THE TITLE SCREEN. The
route (fixtures/play_quit_route.padlog): the s9 beach route, START@2100 (pause menu), UP, CROSS
(Quit game), UP (No -> Yes), CROSS@2340 (the confirm). Arm `quit` closes the window at a boundary past
the confirm (--close 2560) and pins:
  * run.meta load_windows has the title RELOAD window: one starting in [2340, 2470] (the chunk unload +
    the title chunk's load right after the YES) -- the title's reset path ran;
  * run.meta movie_start starts=3 (the two boot movies + the title's intro movie started again by the
    reset) -- StateTitle was reached;
  * the close completed: finalized=1, frames = close or close+1 (the same boundary), `shutdown_ms` < 1000, `shutdown=` carries every
    ladder step (guest_join, savestate_writer, mtvu_worker, graphics_quiesce, effects_quiesce,
    gs_consumer, recorder_flush) and no `guest_join_abandoned`, no watchdog line.
Arm `exit` (RootExitClean) replays the beach session with the close at --close (live gameplay) and
pins the same close contract (shutdown_ms < 1000; every step named; no abandonment; no watchdog).
Red-before: the tester's s10 session ended in `Session shutdown exceeded 10 seconds; terminating`
(finalized=0, no shutdown_ms) -- a run.meta without `shutdown_ms` fails both arms.
SKIP 77 without the title files or a display (video=stub). Selftest pins the judge (no game process).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
CLOSE_LINE = "nativeee: harness.close_at_frame at boundary "
WATCHDOG = "Session shutdown exceeded"
STEPS = ("guest_join", "savestate_writer", "mtvu_worker", "graphics_quiesce", "effects_quiesce",
         "gs_consumer", "recorder_flush")


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


def judge_close(log_text, meta, close, results):
    def check(ok, what):
        results.append((bool(ok), what))
    lines = log_text.splitlines()
    pushed = [l for l in lines if l.startswith(CLOSE_LINE)]
    check(len(pushed) == 1 and pushed[0].startswith(CLOSE_LINE + str(close)) and "SDL_QUIT pushed" in pushed[0],
          "one `%s%d: SDL_QUIT pushed into SDL's queue` line" % (CLOSE_LINE, close))
    check(not any(WATCHDOG in l for l in lines), "no `%s` watchdog line" % WATCHDOG)
    check(meta.get("finalized") == "1", "run.meta finalized=1 (got %s)" % meta.get("finalized"))
    # The close takes effect AT its boundary: the SDL_QUIT pushed by the game thread at
    # boundary `close` is pumped by main while the game thread is between that boundary's
    # consumer and the next boundary's Verify step -- the leg ends with frames = close
    # (the unwind caught the pacer park of close+1) or close+1 (the Verify step of close+1
    # counted first); either is the same boundary. Earlier (the close missed) or later
    # (frames kept running: the s10 freeze shape) is the failure.
    try:
        frames = int(meta.get("frames", "-1"))
    except ValueError:
        frames = -1
    check(close <= frames <= close + 1, "run.meta frames in [%d, %d] (the close took effect at its boundary; got %s)" % (close, close + 1, meta.get("frames")))
    check(meta.get("video") == "gl", "run.meta video=gl (got %s)" % meta.get("video"))
    try:
        ms = int(meta.get("shutdown_ms", "-1"))
    except ValueError:
        ms = -1
    check(0 <= ms < 1000, "run.meta shutdown_ms < 1000 (got %s)" % meta.get("shutdown_ms", "<absent>"))
    census = parse_census(meta.get("shutdown"))
    missing = [s for s in STEPS if s not in census]
    check(not missing, "run.meta shutdown= names every ladder step (missing %s; got %s)" % (missing, meta.get("shutdown", "<absent>")))
    check("guest_join_abandoned" not in census, "no guest worker abandoned (got %s)" % census.get("guest_join_abandoned", "none"))
    check("stuck" not in census, "no step left in progress (got %s)" % census.get("stuck", "none"))


def judge_quit(log_text, meta, close, confirm):
    results = []
    reload = [w for w in windows(meta.get("load_windows")) if confirm - 10 <= w[0] <= confirm + 130]
    results.append((bool(reload), "run.meta load_windows has the title reload window starting in [%d, %d] (got %s)" %
                    (confirm - 10, confirm + 130, meta.get("load_windows", "<absent>"))))
    starts = parse_census(meta.get("movie_start")).get("starts")
    results.append((starts == "3", "run.meta movie_start starts=3 (the title's intro movie restarted by the reset; got %s)" % starts))
    judge_close(log_text, meta, close, results)
    return results


def judge_exit(log_text, meta, close):
    results = []
    judge_close(log_text, meta, close, results)
    return results


def selftest():
    ok = True
    good_log = CLOSE_LINE + "2560: SDL_QUIT pushed into SDL's queue\n"
    good = {"finalized": "1", "frames": "2561", "video": "gl", "shutdown_ms": "17",
            "shutdown": "total=17,game_unwind=1,guest_join=13,savestate_writer=0,mtvu_worker=0,graphics_quiesce=0,effects_quiesce=0,gs_consumer=0,recorder_flush=0",
            "load_windows": "60-111,418-418,531-1174,2440-2560", "movie_start": "selected=1,starts=3,opened=3"}
    r = judge_quit(good_log, good, 2560, 2340)
    ok &= all(x for x, _ in r)
    r = judge_exit(good_log, dict(good, frames="2561"), 2560)
    ok &= all(x for x, _ in r)
    r = judge_exit(good_log, dict(good, frames="2560"), 2560)   # the unwind at the next boundary's park
    ok &= all(x for x, _ in r)
    r = judge_exit(good_log, dict(good, frames="2562"), 2560)   # a close that took a frame to land: FAIL
    ok &= sum(1 for x, _ in r if not x) == 1
    # the s10 shape: the watchdog -- no shutdown_ms, finalized=0, frames short
    bad = {"finalized": "0", "frames": "2500", "video": "gl", "load_windows": "60-111,418-418,531-1174,2440-2500",
           "movie_start": "selected=1,starts=3"}
    r = judge_quit(good_log + WATCHDOG + " 10 seconds; terminating process.\n", bad, 2560, 2340)
    ok &= sum(1 for x, _ in r if not x) == 5
    # a Quit that did not reach the title: no reload window, two movie starts
    r = judge_quit(good_log, dict(good, load_windows="60-111,418-418,531-1174", movie_start="selected=1,starts=2"), 2560, 2340)
    ok &= sum(1 for x, _ in r if not x) == 2
    # an abandoned worker / a stuck step
    r = judge_exit(good_log, dict(good, shutdown=good["shutdown"] + ",guest_join_abandoned=3:entry=0x1000:status=4:wait=1"), 2560)
    ok &= sum(1 for x, _ in r if not x) == 1
    r = judge_exit(good_log, dict(good, shutdown="total=900,game_unwind=1,stuck=guest_join:899"), 2560)
    ok &= sum(1 for x, _ in r if not x) == 2
    print("root_quit_exit selftest: %s" % ("PASS" if ok else "FAIL"))
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
    ap.add_argument("--arm", choices=("quit", "exit"), default="quit")
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--padlog")
    ap.add_argument("--frames", type=int, default=2700)
    ap.add_argument("--close", type=int, default=2560, help="the boundary the window close is pushed at")
    ap.add_argument("--confirm", type=int, default=2340, help="quit arm: the boundary of the YES press in the padlog")
    ap.add_argument("--timeout", type=int, default=400)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner", "padlog"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    name = "RootQuitGame" if args.arm == "quit" else "RootExitClean"
    root = tempfile.mkdtemp(prefix="root_quit_exit_%s_" % args.arm)
    out = os.path.join(root, "leg")
    os.makedirs(out)
    for d in ("mc", "logs"):
        os.makedirs(os.path.join(root, d))
    log = os.path.join(root, "leg.log")
    code = run(args, [args.binary, "--profile", "default", "--harness-window", "--harness-out", out,
                      "--game-files", os.path.abspath(args.game_files), "--frames", str(args.frames),
                      "--inputs", os.path.abspath(args.padlog),
                      "--set", "paths.mc_dir=" + os.path.join(root, "mc"),
                      "--set", "paths.log_dir=" + os.path.join(root, "logs"),
                      "--set", "harness.close_at_frame=%d" % args.close], log, os.path.join(out, "frames.fhash"))
    log_text = open(log).read() if os.path.isfile(log) else ""
    for line in log_text.splitlines():
        if line.startswith(CLOSE_LINE) or WATCHDOG in line or line.startswith("nativeee: session shutdown") or "abandoned" in line:
            print("stderr: " + line[:300])
    meta = parse_meta(os.path.join(out, "run.meta"))
    for key in ("frames", "finalized", "park", "park_detail", "video", "shutdown", "shutdown_ms", "load_windows", "movie_start"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")[:200]))
    if code == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub)")
        shutil.rmtree(root, ignore_errors=True)
        return 77
    results = judge_quit(log_text, meta, args.close, args.confirm) if args.arm == "quit" else judge_exit(log_text, meta, args.close)
    failures = 0
    for ok, what in results:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if code != 0:
        print("  FAIL: runner exit %d (the session must close on its own)" % code)
        failures += 1
    if failures:
        print("%s: FAIL (%d) -- leg kept at %s" % (name, failures, root))
        return 1
    print("%s: PASS (shutdown_ms=%s)" % (name, meta.get("shutdown_ms")))
    if not args.keep:
        shutil.rmtree(root, ignore_errors=True)
    else:
        print("kept at %s" % root)
    return 0


if __name__ == "__main__":
    sys.exit(main())
