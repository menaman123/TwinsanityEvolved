#!/usr/bin/env python3
"""RootWidescreenBeach -- video.aspect=16:9: the title's screen-shape
term is the SELECTED ratio in the PRESENTED frame on the beach -- the scene fills the
width of the anamorphic frame (no pillar/letterbox, no stretch), the projection AND the
culling frustum follow the ratio, the HUD fits are the owner's -- and 4:3 composes
nothing (the deterministic lanes' identity).

Two arms of the tester's s3 recording (tests/app/root_shadow_level_entry.padlog: title ->
New Game -> the first level -> the autosave dialog dismissed at tick 6477 -> Crash idle
on the beach), headless on the RELEASE binary with the deterministic profile's fields
stated one by one, `--frames N` (default 7000) and --dump-frames keeping only p<N>:
  narrow  --set video.aspect=4:3    the reference: the title's own 4:3 term
  wide    --set video.aspect=16:9   the pin
The frame the title draws is the SAME 512x448 for any TV (the console's anamorphic
frame); a 16:9 projection is the 4:3 one with its horizontal term scaled by 3/4 about
the frame's centre (TR view.cpp:34 m[0][0] = cot(fov/2) / (pixelAspect * aspect)), so
the wide frame's central band is the narrow frame squeezed by 0.75 about x = 256, and
its outer bands show world the narrow frame culled. The judge:

  run.meta (wide)
  * frames=N, finalized=1, park=none, graphics_output_faulted=0
  * video_aspect: ratio=16:9, projection>0 with passed_projection=0, frustum>0 with
    passed_frustum=0 (every projection and every culling frustum of the leg took the
    ratio), fit_place>0 and fit_size>0 (the menus' widgets were fit by the owner --
    the HUD anchoring; its arithmetic is pinned bit-for-bit by ScreenAspect), interior=0
  run.meta (narrow)
  * video_aspect=ratio=4:3,model=off   -- 4:3 installs nothing
  pixels (p<N> of both arms, the presenter's output; judged on the 512x448 logical
  frame, a scaled image through the same boxes)
  * squeeze: over x in [64, 448) and y in [100, 448), the mean |Y_wide(x) -
    Y_narrow(256 + (x - 256) / 0.75)| (the narrow frame resampled by 0.75 about the
    centre) is at most 0.6 x the mean |Y_wide(x) - Y_narrow(x)| (the frames compared
    as they are) and at most SQUEEZE_MAX -- the wide frame IS the squeezed scene, not
    the narrow frame (red-before: without the model the two frames are identical, the
    unsqueezed error is 0 and the ratio fails) and not an unrelated picture
  * fills the width: each outer band (x < 64, x >= 448; y in [100, 448)) carries
    content -- luminance standard deviation >= EDGE_STDDEV_MIN (a pillarboxed or boxed
    frame has uniform bars there)
  pixels (p2900 of both arms: the "SAVE SUCCESSFUL" dialog of the route's first save,
  a centred widget disc -- the HUD/widget fit, TR renderer.cpp FitPlaceToScreen /
  FitSizeToScreen owned)
  * anchoring: the disc's horizontal extent on logical row 240 (its blue pixels) is
    0.75 x the narrow arm's (DISC_RATIO +- DISC_RATIO_TOL: a 4:3 TV's ellipse becomes a
    16:9 TV's circle once the frame is stretched -- no stretched HUD) and both centres
    are within DISC_CENTRE_TOL of x = 256 (centres stay centred). Red-before: equal
    extents.

The --selftest judges fake frames: a synthetic beach, its squeezed twin (PASS), the
narrow frame returned as the wide one (the no-model red), a pillarboxed frame, and the
census reds (model=off at 16:9, a passed projection, a missing row).

The legs run with `--set paths.mc_dir=<out>/mc` (a fresh empty card: the recorded
save-menu presses assume empty slots). Verdict rules: FAIL whenever a runner exits
non-zero or run.meta is missing or not finalized=1 or a pin frame was not dumped. SKIP 77
only without the title files or the Release binary.
"""
import argparse
import math
import os
import re
import shutil
import subprocess
import sys
import tempfile
import threading
import time

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")
PADLOG = os.path.join(os.path.dirname(os.path.abspath(__file__)), "root_shadow_level_entry.padlog")
DEFAULT_FRAMES = 7000
DIALOG_FRAME = 2900         # the route's "SAVE SUCCESSFUL" dialog (title -> New Game -> the first save)
DISC_ROW = 240              # a row through the dialog's disc, on the 512x448 logical frame
DISC_RATIO, DISC_RATIO_TOL = 0.75, 0.03
DISC_CENTRE_TOL = 3.0
DETERMINISTIC_FIELDS = (
    "video.interp=off", "video.fps_cap=0", "video.vsync=off",
    "cdvd.latency_model=instant", "cdvd.latency_scale=0", "cdvd.mc_latency_polls=0", "cdvd.load_turbo=off",
    "engine.mtvu=off", "engine.pacing=freerun", "engine.time_regime_pin=1",
    "time.tick_sync=on", "time.tick_deterministic=on", "time.t0_tickpaced=on", "time.maint_eepaced=on",
    "iop.dispatch=frame", "game.rtc_fixed=on", "ui.gui_open=off", "dev.log_level=quiet")
# The 16:9 squeeze of the 4:3 projection about the frame's centre (WideAspect/NarrowAspect = 4/3).
SQUEEZE = 0.75
CENTRE_X = 256.0
BAND_X = (64, 448)          # the wide frame's central band: the narrow frame's whole width squeezed
BAND_Y = (100, 448)         # below the sky
SQUEEZE_RATIO_MAX = 0.6     # squeezed error <= 0.6 x unsqueezed error
SQUEEZE_MAX = 32.0          # and small in absolute terms (texture resampling, not another scene)
EDGE_STDDEV_MIN = 8.0       # content, not a bar, in the outer bands


def parse_meta(path):
    meta = {}
    if not os.path.isfile(path):
        return meta
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def counters(text):
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_-]*)=(-?\d+)", part.strip())
        if m is not None:
            out[m.group(1)] = int(m.group(2))
    return out


def read_ppm(path):
    """P6 -> (w, h, luminance rows top-down as lists of floats). The presented image is top-down."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m is None:
        raise ValueError("not a P6 ppm: %s" % path)
    w, h, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    pixels = data[m.end():]
    if maxval != 255 or len(pixels) < w * h * 3:
        raise ValueError("ppm payload %s: %dx%d maxval %d, %d bytes" % (path, w, h, maxval, len(pixels)))
    rows = []
    for y in range(h):
        row = pixels[y * w * 3:(y + 1) * w * 3]
        rows.append([0.299 * row[3 * x] + 0.587 * row[3 * x + 1] + 0.114 * row[3 * x + 2] for x in range(w)])
    return w, h, rows


def sample(row, x):
    """Linear sample of a luminance row at a fractional x (clamped)."""
    if x <= 0:
        return row[0]
    if x >= len(row) - 1:
        return row[-1]
    i = int(x)
    t = x - i
    return row[i] * (1 - t) + row[i + 1] * t


def disc_extent(path):
    """(left, right, centre) of the dialog disc's blue pixels (b > 150, r < 80) on DISC_ROW, in
    logical 512-wide pixels; None when the row carries no disc."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m is None:
        return None
    w, h = int(m.group(1)), int(m.group(2))
    pixels = data[m.end():]
    sx, sy = w / 512.0, h / 448.0
    y = int(DISC_ROW * sy)
    row = pixels[y * w * 3:(y + 1) * w * 3]
    xs = [x for x in range(w) if row[3 * x + 2] > 150 and row[3 * x] < 80]
    if len(xs) < 8:
        return None
    return min(xs) / sx, max(xs) / sx, (min(xs) + max(xs)) / (2.0 * sx)


def squeeze_stat(narrow, wide):
    """(squeezed error, unsqueezed error) over the central band, on the logical 512x448 frame
    scaled to the images' size (both images must share a size)."""
    w, h, nrows = narrow
    w2, h2, wrows = wide
    if (w, h) != (w2, h2):
        raise ValueError("arms differ in size: %dx%d vs %dx%d" % (w, h, w2, h2))
    sx, sy = w / 512.0, h / 448.0
    squeezed = unsqueezed = 0.0
    count = 0
    cx = CENTRE_X * sx
    for y in range(int(BAND_Y[0] * sy), int(BAND_Y[1] * sy)):
        nrow, wrow = nrows[y], wrows[y]
        for x in range(int(BAND_X[0] * sx), int(BAND_X[1] * sx)):
            source = cx + (x - cx) / SQUEEZE
            squeezed += abs(wrow[x] - sample(nrow, source))
            unsqueezed += abs(wrow[x] - nrow[x])
            count += 1
    return (squeezed / count, unsqueezed / count) if count else (0.0, 0.0)


def edge_stddev(image, left):
    w, h, rows = image
    sx, sy = w / 512.0, h / 448.0
    xs = range(0, int(BAND_X[0] * sx)) if left else range(int(BAND_X[1] * sx), w)
    values = [rows[y][x] for y in range(int(BAND_Y[0] * sy), int(BAND_Y[1] * sy)) for x in xs]
    if not values:
        return 0.0
    mean = sum(values) / len(values)
    return math.sqrt(sum((v - mean) ** 2 for v in values) / len(values))


def judge(frames, narrow_rc, narrow_meta, wide_rc, wide_meta, narrow_frame, wide_frame,
          narrow_dialog=None, wide_dialog=None):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    for arm, rc, meta in (("narrow", narrow_rc, narrow_meta), ("wide", wide_rc, wide_meta)):
        check(rc == 0, "%s: bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % (arm, rc))
        check(meta.get("finalized") == "1", "%s: finalized=1 (finalized=%s)" % (arm, meta.get("finalized", "<absent>")))
        check(meta.get("frames") == str(frames), "%s: frames=%d (frames=%s)" % (arm, frames, meta.get("frames", "<absent>")))
        check(meta.get("park") == "none", "%s: park=none (park=%s park_detail=%s)" % (arm, meta.get("park", "<absent>"), meta.get("park_detail", "<absent>")))
        check(meta.get("graphics_output_faulted") == "0", "%s: graphics_output_faulted=0 (%s)" % (arm, meta.get("graphics_output_faulted", "<absent>")))
    check(narrow_meta.get("video_aspect") == "ratio=4:3,model=off",
          "narrow: video_aspect=ratio=4:3,model=off (4:3 composes nothing; %s)" % narrow_meta.get("video_aspect", "<absent>"))
    aspect = wide_meta.get("video_aspect", "")
    row = counters(aspect)
    check(aspect.startswith("ratio=16:9,"), "wide: video_aspect ratio=16:9 (%s)" % (aspect or "<absent>"))
    check(row.get("projection", 0) > 0 and row.get("passed_projection", -1) == 0,
          "wide: every projection took the ratio (projection=%s passed_projection=%s)"
          % (row.get("projection", "<absent>"), row.get("passed_projection", "<absent>")))
    check(row.get("frustum", 0) > 0 and row.get("passed_frustum", -1) == 0,
          "wide: every culling frustum took the ratio (frustum=%s passed_frustum=%s)"
          % (row.get("frustum", "<absent>"), row.get("passed_frustum", "<absent>")))
    check(row.get("fit_place", 0) > 0 and row.get("fit_size", 0) > 0,
          "wide: the widget fits were the owner's (fit_place=%s fit_size=%s)"
          % (row.get("fit_place", "<absent>"), row.get("fit_size", "<absent>")))
    check("interior" in row and row["interior"] == 0, "wide: interior=0 (%s)" % row.get("interior", "<absent>"))
    images = []
    for arm, path in (("narrow", narrow_frame), ("wide", wide_frame)):
        if not path or not os.path.isfile(path):
            check(False, "%s: presented frame p%d dumped (%s)" % (arm, frames, path or "<absent>"))
            continue
        try:
            image = read_ppm(path)
        except Exception as error:  # noqa: BLE001 -- the verdict names the broken dump
            check(False, "%s: presented frame p%d readable (%s)" % (arm, frames, error))
            continue
        check(image[0] >= 512 and image[1] >= 448, "%s: presented frame p%d is at least 512x448 (%dx%d)" % (arm, frames, image[0], image[1]))
        images.append(image)
    if len(images) == 2:
        try:
            squeezed, unsqueezed = squeeze_stat(images[0], images[1])
            check(squeezed <= SQUEEZE_RATIO_MAX * unsqueezed and squeezed <= SQUEEZE_MAX,
                  "wide frame == the narrow frame squeezed by %.2f about the centre: squeezed error %.2f <= %.1f x unsqueezed %.2f and <= %.0f"
                  % (SQUEEZE, squeezed, SQUEEZE_RATIO_MAX, unsqueezed, SQUEEZE_MAX))
        except ValueError as error:
            check(False, "wide/narrow frames comparable (%s)" % error)
        for left in (True, False):
            dev = edge_stddev(images[1], left)
            check(dev >= EDGE_STDDEV_MIN, "wide frame fills the width: %s band luminance stddev %.1f >= %.0f (no pillarbox)"
                  % ("left" if left else "right", dev, EDGE_STDDEV_MIN))
    discs = []
    for arm, path in (("narrow", narrow_dialog), ("wide", wide_dialog)):
        extent = disc_extent(path) if path and os.path.isfile(path) else None
        check(extent is not None, "%s: dialog frame p%d dumped with the save dialog's disc on row %d (%s)"
              % (arm, DIALOG_FRAME, DISC_ROW, path or "<absent>"))
        discs.append(extent)
    if all(discs):
        (nl, nr, nc), (wl, wr, wc) = discs
        ratio = (wr - wl) / (nr - nl) if nr > nl else 0.0
        check(abs(ratio - DISC_RATIO) <= DISC_RATIO_TOL,
              "widget fit: the dialog disc's width at 16:9 is %.2f x the 4:3 width (%.0f..%.0f vs %.0f..%.0f; want %.2f +- %.2f: no stretched HUD)"
              % (ratio, wl, wr, nl, nr, DISC_RATIO, DISC_RATIO_TOL))
        check(abs(nc - CENTRE_X) <= DISC_CENTRE_TOL and abs(wc - CENTRE_X) <= DISC_CENTRE_TOL,
              "widget fit: the disc stays centred (centre %.1f at 4:3, %.1f at 16:9; within %.0f of %.0f)"
              % (nc, wc, DISC_CENTRE_TOL, CENTRE_X))
    return checks


class FrameKeeper(threading.Thread):
    """Deletes every dumped frame except the pin frame while the leg runs."""

    def __init__(self, directory, keep):
        super().__init__(daemon=True)
        self.directory, self.keep, self.stop = directory, set(keep), threading.Event()

    def sweep(self):
        try:
            names = os.listdir(self.directory)
        except OSError:
            return
        for name in names:
            m = re.fullmatch(r"[fp](\d+)(_drawn_0x[0-9a-fA-F]+)?\.ppm", name)
            if m is None:
                continue
            if m.group(2) or name.startswith("f") or int(m.group(1)) not in self.keep:
                try:
                    os.remove(os.path.join(self.directory, name))
                except OSError:
                    pass

    def run(self):
        while not self.stop.is_set():
            self.sweep()
            time.sleep(0.5)
        self.sweep()


def run_arm(args, name, aspect, out_root):
    out_dir = os.path.join(out_root, name)
    os.makedirs(out_dir)
    log = out_dir + ".log"
    frames_dir = os.path.join(out_dir, "frames")
    os.makedirs(frames_dir)
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", os.path.abspath(args.inputs), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "video.aspect=" + aspect, "--dump-frames", frames_dir]
    for field in DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run %s: %s" % (name, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    keeper = FrameKeeper(frames_dir, (args.frames, DIALOG_FRAME))
    keeper.start()
    proc = subprocess.run(cmd, env=env)
    keeper.stop.set()
    keeper.join()
    print("%s: runner exit=%d" % (name, proc.returncode))
    meta = parse_meta(os.path.join(out_dir, "run.meta"))
    for key in ("frames", "finalized", "wall_ms", "park", "park_detail", "graphics_output_faulted", "video_aspect", "footprint"):
        print("%s: run.meta %s=%s" % (name, key, meta.get(key, "<absent>")))
    return proc.returncode, meta, os.path.join(frames_dir, "p%d.ppm" % args.frames), os.path.join(frames_dir, "p%d.ppm" % DIALOG_FRAME)


def write_ppm(path, w, h, pixel_of):
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        for y in range(h):
            f.write(bytes(c for x in range(w) for c in pixel_of(x, y)))


def fake_scene(x, y):
    """A synthetic beach in WORLD x (the 4:3 frame's pixel grid extended beyond its edges):
    sky above row 100, sand below with a palm trunk, crates and a totem -- structure
    everywhere so a squeeze is measurable."""
    if y < 100:
        return (120, 170, 230)
    r, g, b = 230, 205, 150
    if (int(x) // 24 + y // 24) % 2 == 0:
        r, g, b = 215, 190, 135
    if 215 <= x < 245 and y < 260:
        r, g, b = 110, 70, 30
    if 10 <= x < 130 and 230 <= y < 270:
        r, g, b = 190, 120, 40
    if x >= 400 and 150 <= y < 300:
        r, g, b = 120, 120, 130
    if x < -20 and 180 <= y < 320:
        r, g, b = 60, 140, 60
    if x >= 530 and 300 <= y < 420:
        r, g, b = 60, 60, 160
    return (r, g, b)


def fake_dialog(path, half_width):
    """A dark sea with the save dialog's blue disc (an ellipse of the given half width about
    x = 256, half height 150 about y = 224)."""
    def pixel(x, y):
        dx, dy = (x - 256) / half_width, (y - 224) / 150.0
        if dx * dx + dy * dy <= 1.0:
            return (20, 110, 200)
        return (30, 40, 90)
    write_ppm(path, 512, 448, pixel)


def fake_frames(directory, wide_mode):
    """narrow = the scene at x; wide = the scene at 256 + (x - 256) / 0.75 (the squeezed
    projection sees more world); 'identity' = the narrow frame again (no model);
    'pillarbox' = the narrow frame drawn into 3/4 of the width with black bars."""
    os.makedirs(directory, exist_ok=True)
    narrow = os.path.join(directory, "narrow.ppm")
    wide = os.path.join(directory, "wide.ppm")
    write_ppm(narrow, 512, 448, fake_scene)
    if wide_mode == "squeezed":
        write_ppm(wide, 512, 448, lambda x, y: fake_scene(256 + (x - 256) / SQUEEZE, y))
    elif wide_mode == "identity":
        write_ppm(wide, 512, 448, fake_scene)
    else:
        write_ppm(wide, 512, 448, lambda x, y: (0, 0, 0) if x < 64 or x >= 448 else fake_scene((x - 64) / SQUEEZE, y))
    fake_dialog(os.path.join(directory, "narrow_dialog.ppm"), 179)
    fake_dialog(os.path.join(directory, "wide_dialog.ppm"), 179 if wide_mode == "identity" else 134)
    return narrow, wide


NARROW_META = "frames=7000\nfinalized=1\npark=none\npark_detail=frames_cap\ngraphics_output_faulted=0\nvideo_aspect=ratio=4:3,model=off\n"
WIDE_META = ("frames=7000\nfinalized=1\npark=none\npark_detail=frames_cap\ngraphics_output_faulted=0\n"
             "video_aspect=ratio=16:9,projection=6300,passed_projection=0,frustum=6300,passed_frustum=0,"
             "fit_place=412,passed_fit_place=0,fit_size=380,passed_fit_size=0,interior=0\n")


def selftest():
    script = os.path.abspath(__file__)
    root = tempfile.mkdtemp(prefix="root_widescreen_beach_selftest_")
    ok = True
    try:
        def case(name, wide_mode, narrow_meta, wide_meta, want_rc, want_text, narrow_rc=0, wide_rc=0, drop_wide_frame=False):
            nonlocal ok
            d = os.path.join(root, re.sub(r"\W+", "_", name))
            narrow, wide = fake_frames(d, wide_mode)
            if drop_wide_frame:
                os.remove(wide)
            with open(os.path.join(d, "narrow.meta"), "w") as f:
                f.write(narrow_meta)
            with open(os.path.join(d, "wide.meta"), "w") as f:
                f.write(wide_meta)
            cmd = [sys.executable, script, "--frames", "7000", "--judge-narrow", narrow, os.path.join(d, "narrow.meta"), str(narrow_rc),
                   "--judge-wide", wide, os.path.join(d, "wide.meta"), str(wide_rc),
                   "--judge-dialogs", os.path.join(d, "narrow_dialog.ppm"), os.path.join(d, "wide_dialog.ppm")]
            proc = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
            out = proc.stdout.decode("utf-8", "replace")
            good = proc.returncode == want_rc and want_text in out
            print("  %s %s: exit=%d (want %d)%s" % ("PASS" if good else "FAIL", name, proc.returncode, want_rc,
                                                   "" if want_text in out else ", missing %r" % want_text))
            if not good:
                for line in out.splitlines():
                    print("      | " + line)
                ok = False
        case("the squeezed scene at 16:9", "squeezed", NARROW_META, WIDE_META, 0, "RootWidescreenBeach: ALL PASS")
        case("no model: the wide frame is the narrow frame", "identity", NARROW_META, WIDE_META, 1,
             "FAIL wide frame == the narrow frame squeezed")
        case("no model: the dialog disc keeps its 4:3 width", "identity", NARROW_META, WIDE_META, 1,
             "FAIL widget fit: the dialog disc's width at 16:9 is 1.00 x")
        case("a pillarboxed frame", "pillarbox", NARROW_META, WIDE_META, 1, "FAIL wide frame fills the width: left band")
        case("16:9 with the model off", "squeezed", NARROW_META, WIDE_META.replace(
            "video_aspect=ratio=16:9,projection=6300,passed_projection=0,frustum=6300,passed_frustum=0,fit_place=412,passed_fit_place=0,fit_size=380,passed_fit_size=0,interior=0",
            "video_aspect=ratio=16:9,model=off"), 1, "FAIL wide: every projection took the ratio")
        case("a projection passed to the title's term", "squeezed", NARROW_META,
             WIDE_META.replace("passed_projection=0", "passed_projection=3"), 1, "FAIL wide: every projection took the ratio")
        case("a frustum passed", "squeezed", NARROW_META, WIDE_META.replace("passed_frustum=0", "passed_frustum=1"), 1,
             "FAIL wide: every culling frustum took the ratio")
        case("no widget fit on the route", "squeezed", NARROW_META, WIDE_META.replace("fit_place=412", "fit_place=0"), 1,
             "FAIL wide: the widget fits were the owner's")
        case("4:3 composed a model", "squeezed", NARROW_META.replace("ratio=4:3,model=off", "ratio=4:3,projection=0,passed_projection=6300"),
             WIDE_META, 1, "FAIL narrow: video_aspect=ratio=4:3,model=off")
        case("no video_aspect row", "squeezed", NARROW_META, WIDE_META.replace("video_aspect=", "video_other="), 1,
             "FAIL wide: video_aspect ratio=16:9")
        case("the wide leg crashed", "squeezed", NARROW_META, "frames=0\nfinalized=0\n", 1, "FAIL wide: bounded run exits 0", wide_rc=134)
        case("the wide frame not dumped", "squeezed", NARROW_META, WIDE_META, 1, "FAIL wide: presented frame p7000 dumped", drop_wide_frame=True)
        case("wrong frame cap", "squeezed", NARROW_META, WIDE_META.replace("frames=7000", "frames=6999"), 1, "FAIL wide: frames=7000")
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("root_widescreen_beach selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=DEFAULT_FRAMES)
    ap.add_argument("--timeout", type=int, default=2400, help="per arm")
    ap.add_argument("--inputs", default=PADLOG, help="the route's padlog (PS2_PAD_SCRIPT grammar)")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to both arms as --set KEY=VAL (repeatable)")
    ap.add_argument("--judge-narrow", nargs=3, metavar=("PPM", "META", "RC"), help="judge a dumped narrow arm instead of running it")
    ap.add_argument("--judge-wide", nargs=3, metavar=("PPM", "META", "RC"), help="judge a dumped wide arm instead of running it")
    ap.add_argument("--judge-dialogs", nargs=2, metavar=("NARROW_PPM", "WIDE_PPM"), help="the two arms' dialog frames (with --judge-*)")
    ap.add_argument("--keep", action="store_true", help="keep the harness output directories")
    ap.add_argument("--selftest", action="store_true", help="judge fake frames and censuses (no game process)")
    args = ap.parse_args()
    if args.selftest:
        return selftest()
    out_root = None
    if args.judge_narrow and args.judge_wide:
        dialogs = args.judge_dialogs or (None, None)
        narrow = (int(args.judge_narrow[2]), parse_meta(args.judge_narrow[1]), args.judge_narrow[0], dialogs[0])
        wide = (int(args.judge_wide[2]), parse_meta(args.judge_wide[1]), args.judge_wide[0], dialogs[1])
    else:
        for a in ("binary", "game_files", "runner"):
            if not getattr(args, a):
                print("FAIL: --%s required" % a.replace("_", "-"))
                return 1
        if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
            print("SKIP: title files absent under %s" % args.game_files)
            return 77
        if not os.path.isfile(args.binary):
            print("SKIP: Release binary missing: %s" % args.binary)
            return 77
        if not os.path.isfile(args.inputs):
            print("FAIL: route padlog missing: %s" % args.inputs)
            return 1
        out_root = tempfile.mkdtemp(prefix="root_widescreen_beach_")
        narrow = run_arm(args, "narrow", "4:3", out_root)
        wide = run_arm(args, "wide", "16:9", out_root)
    failures = 0
    for ok, what in judge(args.frames, narrow[0], narrow[1], wide[0], wide[1], narrow[2], wide[2], narrow[3], wide[3]):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootWidescreenBeach: FAIL (%d)%s" % (failures, " -- out %s" % out_root if out_root else ""))
        return 1
    print("RootWidescreenBeach: ALL PASS")
    if out_root and not args.keep:
        shutil.rmtree(out_root, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
