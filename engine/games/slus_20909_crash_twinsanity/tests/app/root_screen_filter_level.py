#!/usr/bin/env python3
"""RootScreenFilterLevel -- the owned colour filter is VISIBLE in the PRESENTED frame.

The bucket-4 screen effects
(TR renderer/effects.cpp DrawScreenEffects) are owned under `hle.graphics_screen`:
the colour filter FilterColours tints every far pixel (Z < 0x10000: the cleared
background and the farthest scenery) toward the chunk's palette entry of its depth
byte by (128 - alpha) / 128 of the way (authored alphas 0x5a..0x7e by index: ~30 %
at index 0 down to ~1 % at the far end). The model path (the retail packets through
the DMAC->VIF->GIF parse and the GS register model's aliasing lanes) is what the
player saw before; this pin judges the owned pass on what the player sees: the
presenter's OUTPUT image.

The leg is the RootShadowLevelEntry route (the tester's s3 recording: title -> New Game
-> the first level -> Crash idle on the beach at frame 7000) with the player's
composition it was measured on (video.scale=2 stated explicitly -- the compiled play
default is 1 -- and the deterministic fields stated one by one) and judges:

  1. the run finishes (exit 0, finalized, no park, no faulted output);
  2. graphics_screen_selection=owned; every DrawScreenEffects call with an effect was
     owned: model_calls=0, refused=0, faults=0, replaced>0, filter>0, unreadable=0;
     the owned deliveries: graphics_screen_scene passes>0, filter_draws>0, failures=0,
     and passes + filter_off == replaced (no blur/wave in retail);
  3. the pin frames: f<key>.ppm (the target grab) and p<key>.ppm (the presented window
     image) both exist, and the FAR band's mean luminance (FAR_BOX: the background
     scenery above the horizon, the frame's only Z < 0x10000 pixels at these frames)
     is at least FAR_LUM_MIN -- a brightness threshold set BETWEEN the two arms'
     measured values (owned 59.3-59.6, model 58.4-58.6). Only luminance is judged;
     the direction of the shift (toward the palette's colour) is NOT tested here --
     no sky (index 0, the 30 % pull) is in view on this route's pin frames, so the
     far trees' 1-12 % pull toward palette 5 (255, 200, 120) brightens the band by
     about one luminance unit over the model arm's weaker aliasing-lane fog. The
     palette index is reported in the result line for the reader, not compared.

Red before (the same binary with `--set hle.graphics_screen=off`, the model arm): the
far band's luminance stays below FAR_LUM_MIN and the census reads model_calls>0 --
both fail (FAIL (8): selection, model_calls, replaced, scene passes, four frame
stats). TRANSITIONAL: the margin is ~1 luminance unit; a route frame with sky in view
(a colour-direction judgement with a 30 % pull) is the follow-up.
`--selftest` judges fake runs.
"""
import argparse
import os
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (the route, the leg mechanics, the ppm reader)

PIN_FRAMES = (7000, 7500)
# The composition the thresholds below were measured on: video.scale=2 (the s4 player's
# composition). The compiled play default is 1, so the leg states 2 explicitly
# (an option, not the default) and judges video_scale=2; the far-band luminance margin is about
# one unit and was not re-measured at scale 1.
VIDEO_SCALE = 2
# The FAR band of the beach frame (top-down logical 512x448 pixels; exclusive upper bounds):
# the background scenery above the horizon (rows 16-160), the frame's only pixels with
# Z < 0x10000 at frames 7000/7500 (no sky is in view there: the far trees take a 1-12 % pull
# toward the chunk's palette 5 colour (255, 200, 120) by their depth byte).
FAR_BOX = (64, 448, 16, 160)
# The eight authored filter palettes' first colours (NTSC g_FilterPalettes 0x2f6c78,
# logical index 0, RGB): what a far pixel is pulled toward. Palette 6 is the identity
# (alpha 0x80: no change) and cannot be pinned. The judgement below uses this table only
# to REFUSE the identity palette and to name the colour in the result line; it does not
# compare the band's colour against it (see the docstring: luminance only, TRANSITIONAL).
PALETTE_FIRST = {0: (0x34, 0x00, 0xf7), 1: (0x00, 0x00, 0x00), 2: (0xa5, 0xeb, 0xff), 3: (0x00, 0xff, 0x12),
                 4: (0xff, 0xff, 0xff), 5: (0xff, 0xc8, 0x78), 7: (0x83, 0x93, 0xca)}
# Measured on this stage's Release (int_work/m_series/graphics/lagrange_2c/{owned,model}):
# FAR_BOX luminance of the presented frame -- owned 59.58 (p7000) / 59.31 (p7500), the model
# arm 58.63 / 58.39 (the model's aliasing lanes draw this fog too, ~1 % weaker, with a bright
# line at the horizon). The pin sits between the arms. It is TRANSITIONAL: the discriminating
# margin is about one luminance unit because no sky (index 0: a 30 % pull) is in view on this
# route's pin frames; a frame with sky in view is the follow-up.
FAR_LUM_MIN = 59.1
# The model arm's measured far-band luminance (p7000), quoted in the result line as the baseline.
MODEL_ARM_LUM = 58.63


def box_mean(rows, box, sx, sy):
    x0, x1, y0, y1 = box
    x0, x1 = int(x0 * sx), int(x1 * sx)
    y0, y1 = int(y0 * sy), int(y1 * sy)
    total = [0, 0, 0]
    n = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            total[0] += row[x * 3]
            total[1] += row[x * 3 + 1]
            total[2] += row[x * 3 + 2]
            n += 1
    return tuple(t / float(n) for t in total) if n else (0.0, 0.0, 0.0)


def luminance(mean):
    return 0.299 * mean[0] + 0.587 * mean[1] + 0.114 * mean[2]


def judge_frame(key, path, palette, presented=False):
    kind = "presented frame p%d" % key if presented else "frame f%d" % key
    if not os.path.isfile(path):
        return [(False, "%s dumped" % kind)]
    try:
        w, h, rows = shadow.read_ppm(path, flip=not presented)
    except ValueError as error:
        return [(False, "%s readable (%s)" % (kind, error))]
    sx, sy = w / 512.0, h / 448.0
    mean = box_mean(rows, FAR_BOX, sx, sy)
    first = PALETTE_FIRST.get(palette)
    if first is None:
        return [(False, "%s far band judged against a non-identity palette (last_palette=%s)" % (kind, palette))]
    lum = luminance(mean)
    # Luminance threshold only (TRANSITIONAL): the owned filter's pull toward palette %d's
    # first colour brightens this band ~1 unit over the model arm; the direction is not judged.
    return [(lum >= FAR_LUM_MIN,
             "%s far band mean (%.1f, %.1f, %.1f): luminance %.2f >= %.1f (threshold between the arms; model arm %.2f; "
             "palette %d first colour %s not compared, TRANSITIONAL) (%dx%d)"
             % (kind, mean[0], mean[1], mean[2], lum, FAR_LUM_MIN, MODEL_ARM_LUM, palette, first, w, h))]


def judge(returncode, meta, frames, frame_paths):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta written at the end of the leg)")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s detail=%s)" % (meta.get("park", "none"), meta.get("park_detail", "")))
    check(meta.get("graphics_output_faulted", "0") == "0", "graphics_output_faulted=0")
    check(meta.get("video_scale") == str(VIDEO_SCALE), "video_scale=%d (the player's composition)" % VIDEO_SCALE)
    check(meta.get("graphics_screen_selection") == "owned", "graphics_screen_selection=owned")
    effects = shadow.counters(meta.get("graphics_screen_effects", ""))
    scene = shadow.counters(meta.get("graphics_screen_scene", ""))
    check(effects.get("calls", 0) > 0 and effects.get("effects", 0) > 0 and effects.get("filter", 0) > 0,
          "graphics_screen_effects calls/effects/filter > 0 (the route reached the level's filter; %s)"
          % meta.get("graphics_screen_effects", "<absent>"))
    check(effects.get("model_calls", 1) == 0, "graphics_screen_effects model_calls=0 (model_calls=%s)" % effects.get("model_calls"))
    check(effects.get("refused", 1) == 0 and effects.get("faults", 1) == 0,
          "graphics_screen_effects refused=0 faults=0 (refused=%s last_refusal=%s faults=%s)"
          % (effects.get("refused"), re.search(r"last_refusal=(\w+)", meta.get("graphics_screen_effects", "")).group(1)
             if "last_refusal=" in meta.get("graphics_screen_effects", "") else "?", effects.get("faults")))
    check(effects.get("replaced", 0) > 0 and effects.get("unreadable", 1) == 0 and effects.get("interior", 1) == 0,
          "graphics_screen_effects replaced>0 unreadable=0 interior=0")
    check(scene.get("passes", 0) > 0 and scene.get("filter_draws", 0) > 0 and scene.get("failures", 1) == 0,
          "graphics_screen_scene passes>0 filter_draws>0 failures=0 (%s)" % meta.get("graphics_screen_scene", "<absent>"))
    if effects.get("blur", 0) == 0 and effects.get("wave", 0) == 0:
        undelivered = effects.get("replaced", -2) - scene.get("passes", -1) - effects.get("filter_off", 0)
        check(0 <= undelivered <= 2,
              "graphics_screen_scene passes + filter_off == replaced (+<=2 pending at the cap): every owned frame "
              "delivered (undelivered=%d)" % undelivered)
    palette = effects.get("last_palette", -1)
    check(palette in PALETTE_FIRST, "last_palette names a pinnable authored palette (last_palette=%s)" % palette)
    for key in PIN_FRAMES:
        results += judge_frame(key, frame_paths[key], palette)
        results += judge_frame(key, frame_paths[("p", key)], palette, presented=True)
    return results


META_KEYS = ("frames", "finalized", "wall_ms", "park", "park_detail", "graphics_output_faulted", "video_scale",
             "graphics_screen_selection", "graphics_screen_effects", "graphics_screen_scene")


def fake_frame(path, colour, presented=False):
    w, h = (512, 448) if presented else (1024, 896)
    shadow.write_ppm(path, w, h, lambda x, y: colour, flip=not presented)


GOOD_META = ("frames=7600\nfinalized=1\nvideo_scale=2\ngraphics_output_faulted=0\ngraphics_screen_selection=owned\n"
             "graphics_screen_effects=calls=5869,empty=0,effects=5869,blur=0,filter=5869,wave=0,model_calls=0,replaced=5869,"
             "filter_off=1731,refused=0,last_refusal=none,faults=0,last_palette=5,last_blends=0,last_height=448,interior=0,"
             "unreadable=0\n"
             "graphics_screen_scene=passes=4138,filter_draws=4138,blur_passes=0,palette_uploads=1,wave_passes=0,"
             "allocations=2,failures=0\n")
MODEL_META = ("frames=7600\nfinalized=1\nvideo_scale=2\ngraphics_output_faulted=0\ngraphics_screen_selection=model\n"
              "graphics_screen_effects=calls=5869,empty=0,effects=5869,blur=0,filter=5869,wave=0,model_calls=5869,replaced=0,"
              "filter_off=0,refused=0,last_refusal=none,faults=0,last_palette=0,last_blends=0,last_height=0,interior=0,"
              "unreadable=0\n"
              "graphics_screen_scene=passes=0,filter_draws=0,blur_passes=0,palette_uploads=0,wave_passes=0,"
              "allocations=0,failures=0\n")


def selftest():
    failures = 0
    tmp = tempfile.mkdtemp(prefix="root_screen_filter_selftest_")
    try:
        model = (69, 59, 30)   # the model arm's far band (luminance 58.6: below FAR_LUM_MIN)
        # the owned arm's far band: the model's colour pulled 3 % toward palette 5 (luminance ~62: above)
        tinted = tuple(int(0.97 * c + 0.03 * p) for c, p in zip(model, PALETTE_FIRST[5]))

        def frames_dir(name, colour, presented_colour=None, keys=PIN_FRAMES, presented_keys=PIN_FRAMES):
            d = os.path.join(tmp, name)
            os.makedirs(d)
            for key in keys:
                fake_frame(os.path.join(d, "f%d.ppm" % key), colour)
            for key in presented_keys:
                fake_frame(os.path.join(d, "p%d.ppm" % key), presented_colour or colour, presented=True)
            return d

        good = frames_dir("good", tinted)
        untinted = frames_dir("untinted", model)
        presented_untinted = frames_dir("presented_untinted", tinted, presented_colour=model)
        missing = frames_dir("missing", tinted, keys=(7000,))
        no_presented = frames_dir("no_presented", tinted, presented_keys=())
        cases = [
            ("the model arm: nothing owned, the far band at the model luminance", 0, MODEL_META, untinted, 1, "FAIL graphics_screen_selection=owned"),
            ("the model arm: model_calls", 0, MODEL_META, untinted, 1, "FAIL graphics_screen_effects model_calls=0"),
            ("the model arm: the presented far band", 0, MODEL_META, untinted, 1, "FAIL presented frame p7000 far band"),
            ("a refusal", 0, GOOD_META.replace("refused=0,last_refusal=none", "refused=3,last_refusal=frame"), good, 1,
             "FAIL graphics_screen_effects refused=0 faults=0 (refused=3 last_refusal=frame faults=0)"),
            ("a failed delivery", 0, GOOD_META.replace("faults=0", "faults=1"), good, 1, "FAIL graphics_screen_effects refused=0 faults=0"),
            ("three undelivered frames", 0, GOOD_META.replace("passes=4138", "passes=4135"), good, 1,
             "FAIL graphics_screen_scene passes + filter_off == replaced"),
            ("two frames pending at the cap", 0, GOOD_META.replace("passes=4138", "passes=4136"), good, 0, "RootScreenFilterLevel: ALL PASS"),
            ("the identity palette", 0, GOOD_META.replace("last_palette=5", "last_palette=6"), good, 1,
             "FAIL last_palette names a pinnable authored palette (last_palette=6)"),
            ("a target grab at the model arm's far-band luminance", 0, GOOD_META, untinted, 1, "FAIL frame f7000 far band"),
            ("a presented frame at the model luminance beside a brightened grab", 0, GOOD_META, presented_untinted, 1, "FAIL presented frame p7000 far band"),
            ("a pin frame not dumped", 0, GOOD_META, missing, 1, "FAIL frame f7500 dumped"),
            ("the presented frame not dumped", 0, GOOD_META, no_presented, 1, "FAIL presented frame p7000 dumped"),
            ("scale 1 composition", 0, GOOD_META.replace("video_scale=2", "video_scale=1"), good, 1, "FAIL video_scale=2"),
            ("crash exit 134", 134, "frames=0\nfinalized=0\n", good, 1, "FAIL bounded run exits 0"),
            ("good run", 0, GOOD_META, good, 0, "RootScreenFilterLevel: ALL PASS"),
        ]
        for name, code, meta_text, frames, expected_rc, expected_line in cases:
            meta = {}
            for line in meta_text.splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    meta[k] = v
            paths = {key: os.path.join(frames, "f%d.ppm" % key) for key in PIN_FRAMES}
            paths.update({("p", key): os.path.join(frames, "p%d.ppm" % key) for key in PIN_FRAMES})
            results = judge(code, meta, 7600, paths)
            lines = ["%s %s" % ("PASS" if ok else "FAIL", what) for ok, what in results]
            rc = 0 if all(ok for ok, _ in results) else 1
            lines.append("RootScreenFilterLevel: ALL PASS" if rc == 0 else "RootScreenFilterLevel: FAIL")
            hit = any(line.startswith(expected_line) for line in lines)
            ok = rc == expected_rc and hit
            print("%s selftest: %s" % ("ok  " if ok else "FAIL", name))
            if not ok:
                failures += 1
                for line in lines:
                    print("    " + line)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootScreenFilterLevelSelftest: %s (%d cases)" % ("PASS" if not failures else "FAIL", len(cases)))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=7600)
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--inputs", default=shadow.PADLOG, help="the route's padlog (PS2_PAD_SCRIPT grammar)")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable; the red arm is hle.graphics_screen=off)")
    ap.add_argument("--frames-from", help="judge the pin frames from this directory instead of dumping")
    ap.add_argument("--meta-from", help="judge this run.meta instead of running (with --frames-from)")
    ap.add_argument("--keep", action="store_true", help="keep the harness output directory")
    ap.add_argument("--selftest", action="store_true", help="judge fake runs (no game process)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    if args.frames_from and args.meta_from:
        meta = shadow.parse_meta(args.meta_from)
        paths = {key: os.path.join(args.frames_from, "f%d.ppm" % key) for key in PIN_FRAMES}
        paths.update({("p", key): os.path.join(args.frames_from, "p%d.ppm" % key) for key in PIN_FRAMES})
        failures = 0
        for ok, what in judge(0, meta, args.frames, paths):
            print("%s %s" % ("PASS" if ok else "FAIL", what))
            failures += 0 if ok else 1
        print("RootScreenFilterLevel: %s" % ("ALL PASS" if not failures else "FAIL (%d)" % failures))
        return 1 if failures else 0
    for a in ("binary", "game_files", "runner"):
        if not getattr(args, a):
            print("FAIL: --%s required" % a.replace("_", "-"))
            return 1
    if not os.path.isfile(os.path.join(args.game_files, shadow.ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77
    if not os.path.isfile(args.inputs):
        print("FAIL: route padlog missing: %s" % args.inputs)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_screen_filter_level_")
    log = out_dir + ".log"
    frames_dir = os.path.join(out_dir, "frames")
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    os.makedirs(frames_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", os.path.abspath(args.inputs), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "video.scale=%d" % VIDEO_SCALE]
    for field in shadow.DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
    cmd += ["--dump-frames", frames_dir]
    for setting in args.settings:
        cmd += ["--set", setting]
    keeper = shadow.FrameKeeper(frames_dir, PIN_FRAMES)
    keeper.start()
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    keeper.stop.set()
    keeper.join()
    print("runner exit=%d" % proc.returncode)
    meta_path = os.path.join(out_dir, "run.meta")
    meta = shadow.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in META_KEYS:
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    paths = {key: os.path.join(frames_dir, "f%d.ppm" % key) for key in PIN_FRAMES}
    paths.update({("p", key): os.path.join(frames_dir, "p%d.ppm" % key) for key in PIN_FRAMES})
    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, paths):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootScreenFilterLevel: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootScreenFilterLevel: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
