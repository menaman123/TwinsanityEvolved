#!/usr/bin/env python3
"""frame_rate_judge.py -- the frozen judge of the FRAME RATE row: per setting (30, 60, 120, 144, 240, uncapped) the hub pad
log in PLAY mode (paced, windowed, scale 1) must SHOW frames at the setting's rate while the GAME runs at 60 Hz and
produces the very same pictures.

    frame_rate_judge.py --bin BUILD/twinsanityevolved --iso <disc image> [--out DIR] [--fixture fixtures/judges/frame_rate_pal.json]
                        [--settings 30,60,120]        # a subset (the fixture's list by default)

The run per setting: `--frame-rate <setting> --play-pad fixtures/padlogs/hub_entry.padlog --skip-movies --trace-state
--stop-at N --harness-out DIR` with an empty `--mc-dir`, NOT `--deterministic` (the presenter paces on the pacer's clock),
`TE_GFX_SUMMARY_EVERY=k` (one `[te.gfx] summary: presents N ...` line per k shown frames; k scales with the setting so the
windows are ~1/6 s), `TE_TICK_LOG_MS=0` (every boundary logged as `tick: boundary N took X ms`: the game's rate is read
off them) and `TE_HARNESS_KEYS=1000,2000,3000` (the harness leg reads back the judged pictures only: a read-back per present
would hold the render thread and measure the judge, not the presenter). Every stderr line is stamped with `time.monotonic()` by this script.

Judged per setting (the fixture freezes the numbers), over the PLAYING span (`game state @ boundary B: Playing` .. the stop):
  * shown fps: the median of the summary windows' presents per wall second -- within +-3 % of the setting; UNCAPPED: at
    least `uncapped_min_fps` (120 on the Apple-silicon Mac it was frozen on), reported as measured. The uncapped run goes first: its shown fps is what
    this host's renderer REACHES, and a setting above it (240 on a renderer that makes 130 interpolated frames a second)
    is judged against that ceiling instead (`judged_against_host_cap` in the results) -- as with vsync on, the display's
    refresh is the cap; the game is never the ceiling;
  * game_hz: boundaries per wall second over the span (the tick lines) -- 60.0 +-0.1;
  * the clock census: `catch-up 0 boundaries, 0 fields dropped` and `0 timeouts` (the game never ran slow or fast);
  * the audio census dropped=0 underruns=0;
  * the run exits 0 at `--stop-at` with no wasm trap.
And across the settings: the REAL pictures' hashes (`frames.fhash`, keyed by the boundary each picture precedes) at the
fixture's fields (1000, 2000, 3000) are IDENTICAL -- the cadence never changes what the game draws; a setting that moved
one pixel of one picture is RED. A runner that refuses `--frame-rate` (the baseline binary: no row) is RED.

Reported, not gated: the presenter's census row (`present=rate:shown_fps:interp_frames:snaps:latency_ms:late_presents`),
the ticks' mean wall / cpu ms (the perf gate's ee_cpu_ms), the clock's `late` count (host load), the window count.
"""
import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "frame_rate_pal.json")

SUMMARY = re.compile(r"^(\d+\.\d+) \[te\.gfx\] summary: presents (\d+) ")
STATE = re.compile(r"^(\d+\.\d+) \[twinsanityevolved\] game state @ boundary (\d+): (\w+) \((\d+)\)")
TICK = re.compile(r"^(\d+\.\d+) \[twinsanityevolved\] tick: boundary (\d+) took ([\d.]+) ms")
CLOCK = re.compile(r"clock: (\d+) boundaries, (\d+) fields, (\d+) parks, (\d+) late, (\d+) timeouts \((\w+) (\d+) Hz\); catch-up (\d+) boundaries, (\d+) fields dropped")
TICKS = re.compile(r"ticks: (\d+) measured, (\d+) over the field \([\d.]+ ms\), max ([\d.]+) ms, mean ([\d.]+) ms(?:, cpu mean ([\d.]+) ms)?")
PRESENT = re.compile(r"\] (present=[^ ]+)(.*)$")
DISP = re.compile(r"displacement checks (\d+) violations (\d+)")
AUDIO = re.compile(r"audio: device=(\d) .*dropped=(\d+) underruns=(\d+)")
STOP = re.compile(r"--stop-at (\d+) reached")


def run(binary, iso, out, fx, setting):
    os.makedirs(out, exist_ok=True)
    mc = os.path.join(out, "mc")
    shutil.rmtree(mc, ignore_errors=True)
    os.makedirs(mc)
    leg = os.path.join(out, "leg")
    shutil.rmtree(leg, ignore_errors=True)
    log = os.path.join(out, "run.log")
    args = [binary, "--iso", iso, "--frame-rate", setting, "--play-pad", os.path.join(ROOT, fx["padlog"]), "--skip-movies",
            "--trace-state", "--stop-at", str(fx["stop_at"]), "--mc-dir", mc, "--run-seconds", str(fx.get("run_seconds", 300)),
            "--config", "none", "--harness-out", leg]
    env = dict(os.environ)
    rate = fx["summary_every"].get(setting, 10)
    env["TE_GFX_SUMMARY_EVERY"] = str(rate)
    env["TE_TICK_LOG_MS"] = "0"
    env["TE_HARNESS_KEYS"] = ",".join(str(k) for k in fx["hash_fields"])   # only the judged pictures are read back: the cadence stays unperturbed
    env.pop("TE_GFX_INTERP_CHECK", None)   # the cadence as a player gets it (the displacement check is interpolation_quality_judge's paced leg)
    env.pop("TE_GFX_HEADLESS", None)   # windowed: the presenter's real cadence on the display
    env.pop("TE_FRAME_RATE", None)
    print("frame_rate_judge: [%s] running %s" % (setting, " ".join(args)), flush=True)
    with open(log, "w") as f:
        p = subprocess.Popen(args, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE, env=env, cwd=ROOT, text=True, errors="replace")
        for line in p.stderr:
            f.write("%.3f %s" % (time.monotonic(), line))
        p.wait()
    return log, leg, p.returncode


def read_hashes(path):
    hashes = {}
    if not os.path.isfile(path):
        return hashes
    with open(path) as f:
        for line in f:
            line = line.strip()
            if not line or "," not in line:
                continue
            key, h = line.split(",", 1)
            try:
                hashes[int(key)] = h.strip()
            except ValueError:
                pass
    return hashes


def judge_log(log, fx, setting, rc):
    m = {"setting": setting, "exit": rc}
    summaries, ticks, states = [], [], []
    with open(log, errors="replace") as f:
        for line in f:
            s = SUMMARY.match(line)
            if s:
                summaries.append((float(s.group(1)), int(s.group(2))))
                continue
            t = TICK.match(line)
            if t:
                ticks.append((float(t.group(1)), int(t.group(2)), float(t.group(3))))
                continue
            st = STATE.match(line)
            if st:
                states.append((float(st.group(1)), int(st.group(2)), st.group(3)))
                continue
            c = CLOCK.search(line)
            if c:
                m["boundaries"], m["fields"] = int(c.group(1)), int(c.group(2))
                m["late"], m["timeouts"] = int(c.group(4)), int(c.group(5))
                m["catchup"], m["dropped"] = int(c.group(8)), int(c.group(9))
                continue
            tk = TICKS.search(line)
            if tk:
                m["tick_mean_ms"] = float(tk.group(4))
                m["tick_cpu_mean_ms"] = float(tk.group(5)) if tk.group(5) else None
                m["ticks_over_field"] = int(tk.group(2))
                continue
            pr = PRESENT.search(line)
            if pr and "[twinsanityevolved]" in line:
                m["present_row"] = pr.group(1)
                m["present_extra"] = pr.group(2).strip()
                d = DISP.search(pr.group(2))
                if d:
                    m["displacement_checks"], m["displacement_violations"] = int(d.group(1)), int(d.group(2))
                continue
            a = AUDIO.search(line)
            if a:
                m["audio_dropped"], m["audio_underruns"] = int(a.group(2)), int(a.group(3))
                continue
            if STOP.search(line):
                m["stop_reached"] = True
            if "wasm trap" in line:
                m["trap"] = True
    # the Playing span: the first Playing at or after the fixture's boundary to the stop
    playing_from = fx["playing_from"]
    t_play = None
    for (ts, b, name) in states:
        if name == "Playing" and b >= playing_from:
            t_play = ts
            break
    m["playing_found"] = t_play is not None
    if t_play is None:
        return m
    # shown fps: the summary windows inside the span
    fps = []
    prev = None
    for (ts, n) in summaries:
        if prev is not None and ts >= t_play and ts - prev[0] > 0:
            fps.append((n - prev[1]) / (ts - prev[0]))
        prev = (ts, n)
    m["windows"] = len(fps)
    if fps:
        fps.sort()
        m["shown_fps_p50"] = round(statistics.median(fps), 1)
        m["shown_fps_p10"] = round(fps[len(fps) // 10], 1)
        m["shown_fps_p90"] = round(fps[(len(fps) * 9) // 10], 1)
    # game_hz: boundaries per wall second over the span (the tick lines)
    span = [(ts, b) for (ts, b, _) in ticks if ts >= t_play]
    if len(span) >= 60:
        (t0, b0), (t1, b1) = span[0], span[-1]
        if t1 > t0:
            m["game_hz"] = round((b1 - b0) / (t1 - t0), 2)
        # the ticks' wall time inside the span
        ms = [x for (ts, _, x) in ticks if ts >= t_play]
        m["span_tick_mean_ms"] = round(statistics.mean(ms), 2)
        m["span_tick_p99_ms"] = round(sorted(ms)[int(len(ms) * 0.99) - 1], 1)
    return m


def verdict(m, fx, setting, host_cap=None):
    """host_cap: the uncapped run's shown fps p50 in this judge run (what this host's renderer reaches); a setting above
    it is judged against the cap -- the display's refresh or the renderer, never the game, is the ceiling (reported)."""
    problems = []
    if m.get("exit") == 2:
        problems.append("the runner refused --frame-rate (no FRAME RATE row)")
        return problems
    if m.get("exit") != 0:
        problems.append("exit %s" % m.get("exit"))
    if m.get("trap"):
        problems.append("a wasm trap")
    if not m.get("stop_reached"):
        problems.append("--stop-at not reached")
    if not m.get("playing_found"):
        problems.append("Playing not reached")
    if m.get("windows", 0) < fx["min_windows"]:
        problems.append("only %d summary windows in the Playing span (< %d)" % (m.get("windows", 0), fx["min_windows"]))
    target = fx["settings"][setting]
    fps = m.get("shown_fps_p50")
    if fps is None:
        problems.append("no shown fps measured")
    elif target == "uncapped":
        if fps < fx["uncapped_min_fps"]:
            problems.append("uncapped shown fps p50 %.1f < %s" % (fps, fx["uncapped_min_fps"]))
    else:
        goal = target
        if host_cap is not None and fx.get("cap_by_uncapped", True) and host_cap < target:
            goal = host_cap   # the renderer's ceiling on this host (the uncapped run measured it)
            m["judged_against_host_cap"] = round(goal, 1)
        tol = fx["fps_tolerance"] * goal
        if fps < goal - tol:
            problems.append("shown fps p50 %.1f not within %.0f%% of %s%s" % (fps, fx["fps_tolerance"] * 100, goal,
                                                                            " (the host's cap; the setting %d)" % target if goal != target else ""))
    hz = m.get("game_hz")
    if hz is None:
        problems.append("no game_hz measured")
    elif abs(hz - 60.0) > fx["game_hz_tolerance"]:
        problems.append("game_hz %.2f not 60.0 +-%.1f" % (hz, fx["game_hz_tolerance"]))
    if m.get("catchup", 0) != 0 or m.get("dropped", 0) != 0:
        problems.append("catch-up %s boundaries, %s fields dropped (the game ran slow or fast)" % (m.get("catchup"), m.get("dropped")))
    if m.get("timeouts", 0) != 0:
        problems.append("%s pacer timeouts" % m.get("timeouts"))
    if m.get("audio_dropped", 0) != 0 or m.get("audio_underruns", 0) != 0:
        problems.append("audio dropped=%s underruns=%s" % (m.get("audio_dropped"), m.get("audio_underruns")))
    return problems


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=None)
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--settings", default=None, help="a comma-separated subset of the fixture's settings")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    out = a.out or os.path.join(ROOT, "build", "frame_rate_judge")
    os.makedirs(out, exist_ok=True)
    settings = list(fx["settings"].keys()) if not a.settings else a.settings.split(",")
    results, hashes = {}, {}
    # uncapped first when it is in the set: its shown fps is this host's ceiling for the settings above it
    if "uncapped" in settings:
        settings = ["uncapped"] + [s for s in settings if s != "uncapped"]
    host_cap = None
    for setting in settings:
        log, leg, rc = run(a.bin, a.iso, os.path.join(out, setting), fx, setting)
        m = judge_log(log, fx, setting, rc)
        m["problems"] = verdict(m, fx, setting, host_cap)
        if setting == "uncapped" and m.get("shown_fps_p50"):
            host_cap = m["shown_fps_p50"]
        results[setting] = m
        hashes[setting] = read_hashes(os.path.join(leg, "frames.fhash"))
        print("frame_rate_judge: [%s] shown fps p50 %s (p10 %s, p90 %s; %s windows), game_hz %s, ticks mean %s ms (cpu %s ms), late %s, catch-up %s, dropped %s, timeouts %s, audio %s/%s, %s %s" % (
            setting, m.get("shown_fps_p50"), m.get("shown_fps_p10"), m.get("shown_fps_p90"), m.get("windows"), m.get("game_hz"),
            m.get("tick_mean_ms"), m.get("tick_cpu_mean_ms"), m.get("late"), m.get("catchup"), m.get("dropped"), m.get("timeouts"),
            m.get("audio_dropped"), m.get("audio_underruns"), m.get("present_row"), m.get("present_extra", "")), flush=True)
    # the pictures' hashes across the settings
    problems = []
    fields = fx["hash_fields"]
    table = {}
    for setting in settings:
        table[setting] = [hashes[setting].get(k) for k in fields]
    for i, k in enumerate(fields):
        values = set(table[s][i] for s in settings if results[s].get("exit") == 0)
        if any(v is None for v in values):
            problems.append("field %d: a setting has no picture hash" % k)
        elif len(values) > 1:
            problems.append("field %d: the pictures differ across the settings (%s)" % (k, ", ".join("%s=%s" % (s, table[s][i]) for s in settings)))
    for setting in settings:
        for p in results[setting]["problems"]:
            problems.append("[%s] %s" % (setting, p))
    with open(os.path.join(out, "results.json"), "w") as f:
        json.dump({"results": results, "hashes": {s: {str(k): v for k, v in zip(fields, table[s])} for s in settings}, "problems": problems}, f, indent=2)
    for setting in settings:
        print("frame_rate_judge: [%s] picture hashes at fields %s: %s" % (setting, fields, table[setting]))
    if problems:
        for p in problems:
            print("frame_rate_judge:", p)
        print("frame_rate_judge: RED (%d problem(s))" % len(problems))
        return 1
    print("frame_rate_judge: GREEN (%s; the pictures at fields %s identical across the settings%s)" % (
        "; ".join("%s -> %.1f fps, %.2f Hz" % (s, results[s]["shown_fps_p50"], results[s]["game_hz"]) for s in settings), fields,
        "; this host's uncapped ceiling %.1f fps caps the settings above it" % host_cap if host_cap else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
