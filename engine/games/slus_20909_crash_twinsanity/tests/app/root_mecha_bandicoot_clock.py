#!/usr/bin/env python3
"""RootMechaBandicootClock -- the clock contract at the first boss: a play-profile,
wall-paced RESUME of the Mecha-Bandicoot savestate (`--load-state mecha_bandicoot_f37165.ps2st
--harness-window --frames N`, the state's video settings passed as --set or the restore refuses
on the provider fingerprint) must hold the game at 60 Hz while the MTVU worker runs the boss's
VU1 programs slower than real time.

The bug (2026-10-13, binary baf15a38): game_hz 61,60,60,45,25,25,26,25,... -- a ~560 ms stall
every ~25 ticks. MtvuBackpressure's BYTES PARK: the worker (the boss programs the title
library misses, on the cached tier, ~40% of real time) fell behind, the pipeline filled to the
256 MB high-water mark and the GAME THREAD parked until 32 MB drained (bp=39 engagements in
1200 frames). The fix: in play mode the game thread never parks on the worker -- a frame the
worker is > 2 frames behind runs its VIF stream without its VU1 programs (the KICK DROP,
engine.mtvu_kick_drop=on; run.meta `mtvu=` kick_drop/drop/kicks_dropped/lag_max).

Judged (run.meta):
  - runner exit 0, finalized=1, video=gl, frames=N;
  - rule 5: game_hz 59.5..60.5 in EVERY window but the first and last (no in-loop filter: the
    resume is all frame loop -- the >= 30-boundary filter is what let baf15a38's 25 Hz windows
    past RootClockContract's band); at most --max-own-time-windows (1) windows dented to
    58..59.5 by a long tick (fields > boundaries);
  - rule 2: gs_queue frame_waits=0, frame_skip=1; frames_dropped published;
  - the worker never parked the game thread: `mtvu=` bp engagements = 0, kick_drop=on, and the
    tick census site `vu1wait` booked 0 us (`tick_profile` row -- absent on baf15a38: red);
  - REPORT: drop frames / kicks dropped / lag_max, render_fps min/max, pace_late, pace_work_max_ms.

Exit 77 (SKIP) without the title files or the state, or when the run COMPLETED and reported
video=stub. Needs an idle machine. `--selftest` judges canned run.meta values.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
# The state was saved with these video settings; the restore's provider fingerprint requires them.
STATE_SETS = ["video.anisotropy=16", "video.aspect=21:9", "video.texture_filter=point", "video.vsync=on"]


def parse_meta(path):
    meta = {}
    with open(path) as f:
        for line in f:
            line = line.rstrip("\n")
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k] = v
    return meta


def field(row, name):
    m = re.search(r"(?:^|,)%s=(\d+)" % re.escape(name), row or "")
    return int(m.group(1)) if m else -1


def field_str(row, name):
    m = re.search(r"(?:^|,)%s=([^,]*)" % re.escape(name), row or "")
    return m.group(1) if m else ""


def series(s):
    return [int(x) for x in (s or "").split(",") if re.fullmatch(r"\d+", x)]


def judge(returncode, meta, frames, max_own_time_windows=1):
    checks = []

    def check(ok, what):
        checks.append((ok, what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("video") == "gl", "video=gl (video=%s)" % meta.get("video", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(re.fullmatch(r"\d+", meta.get("restored_vsync_frame", "")) is not None,
          "the state restored (restored_vsync_frame=%s)" % meta.get("restored_vsync_frame", "<absent>"))
    gq = meta.get("gs_queue", "")
    check(field(gq, "frame_waits") == 0, "rule 2: gs_queue frame_waits=0 (%d)" % field(gq, "frame_waits"))
    check(field(gq, "frame_skip") == 1, "rule 2: gs_queue frame_skip=1 (%d)" % field(gq, "frame_skip"))
    check(re.fullmatch(r"\d+", meta.get("frames_dropped", "")) is not None,
          "rule 2: frames_dropped published (%s)" % meta.get("frames_dropped", "<absent>"))
    mt = meta.get("mtvu", "")
    bp = field_str(mt, "bp")
    engagements = int(bp.split("/")[0]) if re.match(r"\d+/", bp) else -1
    check(engagements == 0, "the worker never parked the game thread: mtvu bp engagements=0 (bp=%s)" % (bp or "<absent>"))
    check(field_str(mt, "kick_drop") == "on", "the kick drop is on (mtvu kick_drop=%s)" % (field_str(mt, "kick_drop") or "<absent>"))
    tp = meta.get("tick_profile", "")
    vu1wait = field_str(tp, "vu1wait")
    vu1wait_us = int(vu1wait.split("/")[0]) if re.match(r"\d+/", vu1wait) else -1
    check(vu1wait_us == 0, "tick census: vu1wait booked 0 us (the site named the park; tick_profile vu1wait=%s -- the row is absent on baf15a38)"
          % (vu1wait or "<absent>"))
    game_hz, field_hz, render_fps = series(meta.get("game_hz")), series(meta.get("field_hz")), series(meta.get("render_fps"))
    check(len(game_hz) > 0 and len(field_hz) == len(game_hz) and len(render_fps) == len(game_hz),
          "rule 5: game_hz / field_hz / render_fps series published (%d windows)" % len(game_hz))
    # EVERY window but the first (the restore's partial second) and the last (partial) is
    # judged: the resume runs the boss's frame loop throughout -- no load, no FMV -- so the
    # clock contract's in-loop filter (>= 30 boundaries) does not apply here; that filter
    # is what let the 25 Hz windows of baf15a38 slip past RootClockContract's band.
    rows = list(zip(game_hz, field_hz, render_fps))
    judged = rows[1:-1]
    check(len(judged) >= 10, "the boss ran >= 10 full windows (%d)" % len(judged))
    if judged:
        # One long tick (fields > boundaries, the EE's own work) may dent a window to 58..59.5;
        # anything lower, or more such windows than --max-own-time-windows, is the stall.
        own_time = [(g, f) for g, f, _ in judged if f > g and 58 <= g < 59.5]
        out_of_band = [g for g, f, _ in judged if not (59.5 <= g <= 60.5) and (g, f) not in own_time]
        check(not out_of_band, "DONE band: game_hz 59.5..60.5 in EVERY window (out of band: %s; baf15a38 read 25)"
              % (out_of_band or "none"))
        check(len(own_time) <= max_own_time_windows,
              "EE_OWN_TIME windows (fields > boundaries, game_hz 58..59.5) = %d <= %d: %s"
              % (len(own_time), max_own_time_windows, ["game_hz=%d field_hz=%d" % gf for gf in own_time] or "none"))
        gmin, gmax = min(g for g, _, _ in judged), max(g for g, _, _ in judged)
        rmin, rmax = min(r for _, _, r in judged), max(r for _, _, r in judged)
        checks.append((True, "REPORT windows=%d game_hz min/max=%d/%d render_fps min/max=%d/%d; mtvu drop=%s kicks_dropped=%s lag_max=%s; "
                             "frames_dropped=%s pace_late=%s pace_late_fields=%s pace_work_max_ms=%s vu1_hle_worker=%s"
                       % (len(judged), gmin, gmax, rmin, rmax, field_str(mt, "drop"), field_str(mt, "kicks_dropped"), field_str(mt, "lag_max"),
                          meta.get("frames_dropped"), meta.get("pace_late"), meta.get("pace_late_fields"), meta.get("pace_work_max_ms"),
                          meta.get("vu1_hle_worker"))))
    return checks


GOOD_META = {
    "frames": "1200", "finalized": "1", "video": "gl", "restored_vsync_frame": "37165",
    "gs_queue": "mode=thread,policy=pipelined,enqueued=2334965,consumed=2334965,frames=1200,syncs=0,frame_waits=0,max_depth=6032,frame_skip=1,"
                "ordered_waits=0,ordered_wait_ms=3,sync_wait_ms=0",
    "frames_dropped": "338",
    "mtvu": "on,worker=1,drain=0,drain_thread=inline,redirects=0,boundaries=1200/0,declared=1200,presented=1200,batches=299585,"
            "stage_cap=307504,pending_bytes=3687392,bp=0/0/0,quiesce_laps=0,mirrors=3,kick_drop=on,drop=749/4237619/150/0,"
            "kicks_dropped=1276623,lag_max=6,frame_marks=1200/1198",
    "tick_profile": "loader=0/0,tex=0/0,mesh=120/465719,shadow=0/0,sky=0/0,save=0/0,alloc=113/193786,gs=12/2400,boundary=2583/9593,vu1wait=0/0",
    "game_hz": "61,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60",
    "field_hz": "60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60",
    "render_fps": "58,43,45,42,42,42,42,42,41,42,43,42,42,41,41,44,42,43,42",
    "pace_late": "2", "pace_late_fields": "8", "pace_work_max_ms": "19", "vu1_hle_worker": "812/770162/0",
}

# baf15a38's run.meta (a reference leg): the park.
RED_META = {
    "frames": "1200", "finalized": "1", "video": "gl", "restored_vsync_frame": "37165",
    "gs_queue": "mode=thread,policy=pipelined,enqueued=5014432,consumed=5014432,frames=1200,syncs=0,frame_waits=0,max_depth=3630,frame_skip=1,"
                "ordered_waits=0,ordered_wait_ms=5,sync_wait_ms=0",
    "frames_dropped": "5",
    "mtvu": "on,worker=1,drain=0,drain_thread=inline,redirects=0,boundaries=1200/0,declared=1200,presented=1200,batches=270440,"
            "stage_cap=307504,pending_bytes=259280416,bp=39/0/0,quiesce_laps=0,mirrors=3",
    "game_hz": "61,60,60,45,25,25,26,25,26,26,26,27,26,27,26,26,25,27,27,26,27,26,25,28,27,27,27,27,26,24,21,22,24,23,23,24,26,26,26,27,26",
    "field_hz": "60,60,60,45,57,57,58,59,58,58,59,61,59,61,59,59,57,93,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60,60",
    "render_fps": "60,59,59,46,25,25,25,25,26,26,26,27,26,27,26,25,25,25,29,24,28,26,25,28,25,28,26,27,27,23,22,21,25,23,23,24,26,26,26,27,26",
}


def selftest():
    fails = 0

    def expect(name, meta, want, rc=0):
        nonlocal fails
        got = all(c for c, _ in judge(rc, meta, 1200))
        print("selftest %s: %s" % (name, "PASS" if got == want else "FAIL"))
        fails += got != want

    expect("good meta passes", GOOD_META, True)
    expect("baf15a38 leg4 meta fails (game_hz 25, bp=39, no tick_profile/kick_drop)", RED_META, False)
    bad = dict(GOOD_META); bad["game_hz"] = "61,60,60,60,45,25,25,60,60,60,60,60,60,60,60,60,60,60,60"
    expect("game_hz 45/25 windows fail (fields > boundaries is no excuse below 58)", bad, False)
    bad = dict(GOOD_META); bad["game_hz"] = "61,60,60,60,59,60,60,60,60,60,60,60,60,60,60,60,60,60,60"
    expect("one own-time window at 59 passes", bad, True)
    bad = dict(GOOD_META); bad["game_hz"] = "61,60,60,60,59,60,59,60,60,60,60,60,60,60,60,60,60,60,60"
    expect("two own-time windows fail", bad, False)
    bad = dict(GOOD_META); bad["mtvu"] = bad["mtvu"].replace("bp=0/0/0", "bp=3/0/0")
    expect("a backpressure engagement fails", bad, False)
    bad = dict(GOOD_META); bad["mtvu"] = bad["mtvu"].replace("kick_drop=on", "kick_drop=off")
    expect("kick_drop=off fails", bad, False)
    bad = dict(GOOD_META); bad["tick_profile"] = bad["tick_profile"].replace("vu1wait=0/0", "vu1wait=560000/1")
    expect("vu1wait booked fails", bad, False)
    bad = dict(GOOD_META); del bad["tick_profile"]
    expect("tick_profile row absent fails", bad, False)
    bad = dict(GOOD_META); bad["gs_queue"] = bad["gs_queue"].replace("frame_waits=0", "frame_waits=4")
    expect("frame_waits=4 fails", bad, False)
    expect("crash exit fails", GOOD_META, False, rc=134)
    bad = dict(GOOD_META); del bad["restored_vsync_frame"]
    expect("no restore fails", bad, False)
    return 1 if fails else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--state", help="the Mecha-Bandicoot savestate (.ps2st)")
    ap.add_argument("--frames", type=int, default=1200)
    ap.add_argument("--timeout", type=int, default=240)
    ap.add_argument("--keep", help="copy run.meta here")
    ap.add_argument("--max-own-time-windows", type=int, default=1)
    ap.add_argument("--extra-set", action="append", default=[], help="additional --set KEY=VALUE for the leg")
    ap.add_argument("--selftest", action="store_true")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    for a in ("binary", "game_files", "runner", "state"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.state):
        print("SKIP: the Mecha-Bandicoot state is absent: %s" % args.state)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix="root_mecha_bandicoot_clock_")
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir)
    leg = os.path.join(out_dir, "leg")
    os.makedirs(leg)
    log = out_dir + ".log"
    cmd = [args.runner, str(args.timeout), log, "--", args.binary, "--profile", "default", "--harness-window",
           "--harness-out", leg, "--frames", str(args.frames), "--load-state", os.path.abspath(args.state),
           "--game-files", os.path.abspath(args.game_files), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.log_dir=" + os.path.join(out_dir, "logs")]
    for s in STATE_SETS + args.extra_set:
        cmd += ["--set", s]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(leg, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(leg, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "video", "wall_ms", "restored_vsync_frame", "mtvu", "gs_queue", "frames_dropped", "pace_late",
                "pace_late_fields", "pace_late_trace", "pace_work_max_ms", "tick_profile", "game_hz", "field_hz", "render_fps",
                "vu1_hle_worker"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")[:600]))
    if args.keep and os.path.isfile(meta_path):
        os.makedirs(args.keep, exist_ok=True)
        shutil.copy(meta_path, os.path.join(args.keep, "run.meta"))
    if proc.returncode == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display (see %s)" % log)
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    checks = judge(proc.returncode, meta, args.frames, args.max_own_time_windows)
    ok = True
    for passed, what in checks:
        print("%s %s" % ("PASS" if passed else "FAIL", what))
        ok = ok and passed
    if ok:
        shutil.rmtree(out_dir, ignore_errors=True)
        if os.path.isfile(log):
            os.remove(log)
    else:
        print("artifacts kept: %s (log %s)" % (out_dir, log))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
