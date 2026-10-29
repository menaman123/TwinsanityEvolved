#!/usr/bin/env python3
"""RootShadowLevelEntry -- the first level's default shadow meshes are ACCEPTED and
DRAWN by the owned shadow pass (hle.graphics_shadows): a visible dark footprint under
Crash, no model-body strip into the scene, no owned draw for a pass with no mesh.

Play session s3 (all-on Release 2026-10-09 19:47:57): no shadow rendered and a tall
white vertical streak was drawn over Crash. run.meta: `graphics_shadow_mesh model_calls=
18728 replaced=0`, `graphics_shadow instances=0 refused_meshes=18728 last_mesh_refusal=
check:4/result:2` (Shape: the owner refused every default mesh because its shader
answers NeedsEye -- which every type-0B shader does), and the one-mesh model body ran
INSIDE the owned bracket, whose begin had bypassed StartShadows' half-size target: the
model's volume strips (colour 8/248, additive) landed in the scene = the streak. Then
(this 's diag leg) check 9 (View): the matrix arguments are DrawShadowEntry's
locals on the title's scratchpad stack (0x7000xxxx), which the owner did not read.

Play session s4 (all-on Release 96c8525c, 2026-10-10, video_scale=2): the pass accepted
everything (`graphics_shadow instances=19398 refused=0`, `graphics_shadow_scene published=
1130`) and this pin -- then on the deterministic profile, i.e. video.scale=1, judging the
target grab -- passed, yet the player saw NO shadow. At scale 2 the renderer's private
half-size images were allocated at the scene's scale while the backend's strip-count pass
rasterises natively: the volumes landed at half size in the top-left quarter of the
accumulator and the composite put nothing under Crash (sand between the feet 133.3 vs
ring 144.5 at f7000 = the shadow-off value). The pin therefore runs the PLAYER'S
composition (the deterministic fields stated one by one, `--profile deterministic` would
pin scale 1 after them, plus video.scale=2) and judges the presenter's OUTPUT (p<key>.ppm,
the window image after the display quad) beside the target grab (f<key>.ppm).

This pin replays the tester's s3 recording (tests/app/root_shadow_level_entry.padlog) for
--frames N (default 7600: the autosave dialog is dismissed at tick 6477; Crash then stands
idle on the beach) with --dump-frames, keeping only the pin frames, and judges:

  run.meta
  * frames=N, finalized=1, park=none, graphics_output_faulted=0
  * graphics_shadow_selection=owned            -- the pin's premise
  * graphics_shadow_mesh calls>0, model_calls=0, replaced==calls
        -- the route REACHED the default meshes (calls=0 is a route failure, never a
           pass); NO model body ever runs inside an owned bracket (the streak's arm)
  * graphics_shadow instances>0, blocks>0, groups>0, faults=0, refused_meshes=0
        -- every default mesh was accepted and grouped; a remaining refusal FAILS with
           its class named (last_mesh_refusal check/result)
  * graphics_shadow_scene published>0 and published+empty == graphics_shadow completed
        -- every delivered pass either drew (volumes) or published NOTHING (no volume);
           graphics_shadow empty == graphics_shadow_scene empty + pending(<=2)
  * graphics_refusals_shadows total==0, faults=0
  * video_scale=<--video-scale>                  -- the player's composition (default 1,
        the compiled play default; the s4 bug is the scale-2 arm)
  pixels (f7000/f7500 = the target grab, 512x448 at scale 1, 1024x896 at scale 2; p7000/p7500 =
  the presented window image; the 512x448 boxes below are scaled to each image's size)
  * footprint: mean luminance of the sand between Crash's feet (x 246..262, y 386..402,
    top-down) is at least 22 below the sand ring beside him (x 180..210 and 290..320, same
    rows): shadow on = -36, shadow off = -10 on this route (diag legs of 2026-10-09)
  * streak: no column of the lower 2/3 of the frame carries a vertical run of >= 40 pixels
    with min(r,g,b) >= 230 (the s3 streak; the sand is ~(230,205,150), the sky is above)

The --selftest judges fake run.meta/frames: the s3 session's census is the red case
(model_calls, refused Shape, instances=0), as is a frame with a white streak or no footprint.

The leg runs with `--set paths.mc_dir=<out>/mc` (a fresh empty card: the recorded
save-menu presses assume empty slots, and the level's autosave must not fill the shared
default `memcards/`, which moves the next run's cursor -- a leg of 2026-10-09 diverged so).

Verdict rules: FAIL whenever the runner exits non-zero (a crash, SIGABRT 134, a kill by
run_bounded.sh 124/125/137) or run.meta is missing or not finalized=1 or a pin frame was
not dumped. SKIP 77 only without the title files or the Release binary.
"""
import argparse
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
PIN_FRAMES = (7000, 7500)
# The player's composition: the deterministic profile's fields stated one by one
# (`--profile deterministic` would pin video.scale=1 AFTER every --set) plus the player's
# video.scale (`--video-scale`). Since 2026-10-11 the compiled play default is
# scale 1 (DEFAULT_VIDEO_SCALE); scale 2 stays an option and the s4 bug reproduces ONLY
# there, so the suite runs both arms (RootShadowLevelEntry at 1, RootShadowLevelEntryScale2).
# The pixel boxes are stated on the 512x448 logical frame and scaled to each image's size,
# so the judge is the same at every scale.
DEFAULT_VIDEO_SCALE = 1
VIDEO_SCALE = DEFAULT_VIDEO_SCALE
DETERMINISTIC_FIELDS = (
    "video.interp=off", "video.fps_cap=0", "video.vsync=off",
    "cdvd.latency_model=instant", "cdvd.latency_scale=0", "cdvd.mc_latency_polls=0", "cdvd.load_turbo=off",
    "engine.mtvu=off", "engine.pacing=freerun", "engine.time_regime_pin=1",
    "time.tick_sync=on", "time.tick_deterministic=on", "time.t0_tickpaced=on", "time.maint_eepaced=on",
    "iop.dispatch=frame", "game.rtc_fixed=on", "ui.gui_open=off", "dev.log_level=quiet")
# Top-down pixel boxes (x0, x1, y0, y1), exclusive upper bounds, on the 512x448 logical
# frame; a dumped image of another size (the target grab at scale 2 = 1024x896, the
# presented window image) is judged through the same boxes scaled to its size.
FOOT_BOX = (246, 262, 386, 402)
RING_BOXES = ((180, 210, 386, 402), (290, 320, 386, 402))
FOOT_DARKER_BY = 22.0
# Crash himself (the skinned + blend-shape VU1 families the route exercises natively: the
# `classes skinned` / `indexed-displace` draws) stands over the footprint: his fur fills
# this box of the presented frame (the kept pin frames read (125, 45, 4) and (116, 43, 4);
# the sand beside him (170, 138, 75)). Orange: red >= FUR_RED_MIN, green <= 0.6 red, blue
# <= 0.25 red. The presented-frame pin of the skin/blend-shape draws.
BODY_BOX = (238, 270, 290, 322)
FUR_RED_MIN = 90.0
# The route's resident peak bound (main's deterministic level legs read 490-560 MB; the
# 2a runaway read 1654 and 3020) and the VU1 library miss share bound (runs/misses: the
# model arm reads ~1e-5, the residency bug read 1/10).
PEAK_MB_MAX = 1000
VU1_MISS_SHARE_DENOMINATOR = 1000


def vu1_library(meta):
    """(runs, misses) of the VU1 library consult: the inline unit's row (vu1_hle=runs/misses/
    declines) plus the MTVU worker's (vu1_hle_worker) when it ran."""
    runs = misses = 0
    for key in ("vu1_hle", "vu1_hle_worker"):
        parts = meta.get(key, "").split("/")
        if len(parts) == 3 and all(part.isdigit() for part in parts):
            runs += int(parts[0])
            misses += int(parts[1])
    return runs, misses
STREAK_MIN_RUN, STREAK_MIN_CHANNEL, STREAK_FROM_ROW = 40, 230, 150


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.strip()
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def counters(text):
    """`a=1,b=2,x=word,...` -> {a: 1, b: 2}; non-numeric fields are skipped."""
    out = {}
    for part in text.split(","):
        m = re.fullmatch(r"([A-Za-z_][A-Za-z0-9_-]*)=(-?\d+)", part.strip())
        if m is not None:
            out[m.group(1)] = int(m.group(2))
    return out


def read_ppm(path, flip=True):
    """P6 -> (width, height, rows top-down of [(r, g, b), ...]). The target grab f<key>
    is written in the attachment's bottom-up row order and is flipped here to top-down;
    the presented image p<key> (the window after the display quad) is already top-down."""
    with open(path, "rb") as f:
        data = f.read()
    m = re.match(rb"P6\s+(\d+)\s+(\d+)\s+(\d+)\s", data)
    if m is None:
        raise ValueError("not a P6 ppm: %s" % path)
    w, h, maxval = int(m.group(1)), int(m.group(2)), int(m.group(3))
    pixels = data[m.end():]
    if maxval != 255 or len(pixels) < w * h * 3:
        raise ValueError("ppm payload %s: %dx%d maxval %d, %d bytes" % (path, w, h, maxval, len(pixels)))
    rows = [pixels[y * w * 3:(y + 1) * w * 3] for y in range(h)]
    if flip:
        rows.reverse()
    return w, h, rows


def scaled(box, sx, sy):
    x0, x1, y0, y1 = box
    return (int(x0 * sx), int(x1 * sx), int(y0 * sy), int(y1 * sy))


def luminance_mean(rows, box):
    x0, x1, y0, y1 = box
    total, count = 0.0, 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            r, g, b = row[x * 3], row[x * 3 + 1], row[x * 3 + 2]
            total += 0.299 * r + 0.587 * g + 0.114 * b
            count += 1
    return total / count if count else 0.0


def box_mean(rows, box):
    """Mean (r, g, b) of a top-down pixel box."""
    x0, x1, y0, y1 = box
    r = g = b = n = 0
    for y in range(y0, min(y1, len(rows))):
        row = rows[y]
        for x in range(x0, min(x1, len(row) // 3)):
            r += row[3 * x]
            g += row[3 * x + 1]
            b += row[3 * x + 2]
            n += 1
    return (r / n, g / n, b / n) if n else (0.0, 0.0, 0.0)


def fur(mean):
    r, g, b = mean
    return r >= FUR_RED_MIN and g <= 0.6 * r and b <= 0.25 * r


def footprint_stat(rows, sx=1.0, sy=1.0):
    """(foot mean, ring mean): the sand between Crash's feet against the sand beside him."""
    foot = luminance_mean(rows, scaled(FOOT_BOX, sx, sy))
    a, b = scaled(RING_BOXES[0], sx, sy), scaled(RING_BOXES[1], sx, sy)
    ring = (luminance_mean(rows, a) * (a[1] - a[0]) +
            luminance_mean(rows, b) * (b[1] - b[0])) / ((a[1] - a[0]) + (b[1] - b[0]))
    return foot, ring


def streak_run(w, h, rows, sy=1.0):
    """Longest vertical run (rows >= STREAK_FROM_ROW) of pixels with min(r,g,b) >= STREAK_MIN_CHANNEL,
    and the column it is in; the run length is reported in logical (448-row) pixels."""
    best, best_x = 0, -1
    for x in range(w):
        run = 0
        for y in range(int(STREAK_FROM_ROW * sy), h):
            row = rows[y]
            if min(row[x * 3], row[x * 3 + 1], row[x * 3 + 2]) >= STREAK_MIN_CHANNEL:
                run += 1
                if run > best:
                    best, best_x = run, x
            else:
                run = 0
    return int(best / sy), best_x


def judge_frame(key, path, presented=False):
    """One dumped image: the target grab f<key> (the frames lane, bottom-up, 512*scale wide)
    or the presenter's output p<key> (the window image the player sees, top-down, any size)."""
    name = "%s%d" % ("p" if presented else "f", key)
    what = "presented frame" if presented else "frame"
    checks = []
    if not path or not os.path.isfile(path):
        checks.append((False, "%s %s dumped (%s)" % (what, name, path or "<absent>")))
        return checks
    try:
        w, h, rows = read_ppm(path, flip=not presented)
    except Exception as error:  # noqa: BLE001 -- the verdict names the broken dump
        checks.append((False, "%s %s readable (%s)" % (what, name, error)))
        return checks
    if presented:
        ok = w >= 512 and h >= 448
    else:
        ok = (w, h) == (512 * VIDEO_SCALE, 448 * VIDEO_SCALE)
    if not ok:
        checks.append((False, "%s %s is %s (%dx%d)" % (what, name, "at least 512x448" if presented
                       else "%dx%d" % (512 * VIDEO_SCALE, 448 * VIDEO_SCALE), w, h)))
        return checks
    sx, sy = w / 512.0, h / 448.0
    foot, ring = footprint_stat(rows, sx, sy)
    checks.append((foot <= ring - FOOT_DARKER_BY,
                   "%s %s dark footprint under Crash: sand between the feet %.1f <= ring %.1f - %.0f (%dx%d)"
                   % (what, name, foot, ring, FOOT_DARKER_BY, w, h)))
    body = box_mean(rows, scaled(BODY_BOX, sx, sy))
    checks.append((fur(body),
                   "%s %s Crash drawn over his footprint: fur (orange, red >= %.0f, green <= 0.6 red, blue <= 0.25 red) in the body box, read (%.1f, %.1f, %.1f)"
                   % (what, name, FUR_RED_MIN, body[0], body[1], body[2])))
    run, x = streak_run(w, h, rows, sy)
    checks.append((run < STREAK_MIN_RUN,
                   "%s %s no white vertical streak: longest near-white run %d px (column %d) < %d"
                   % (what, name, run, x, STREAK_MIN_RUN)))
    return checks


def judge(returncode, meta, frames, frame_paths):
    checks = []

    def check(ok, what):
        checks.append((bool(ok), what))

    check(returncode == 0, "bounded run exits 0 (runner exit=%d; non-zero = crash/kill, never a skip)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1 (run.meta finalized=%s)" % meta.get("finalized", "<absent>"))
    check(meta.get("frames") == str(frames), "frames=%d (the --frames cap ended the leg; frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park") == "none", "park=none (park=%s park_detail=%s)" % (meta.get("park", "<absent>"), meta.get("park_detail", "<absent>")))
    check(meta.get("graphics_output_faulted") == "0", "graphics_output_faulted=0 (%s)" % meta.get("graphics_output_faulted", "<absent>"))
    check(meta.get("graphics_shadow_selection") == "owned",
          "graphics_shadow_selection=owned (the owned pass is composed; %s)" % meta.get("graphics_shadow_selection", "<absent>"))
    mesh = counters(meta.get("graphics_shadow_mesh", ""))
    calls = mesh.get("calls", 0)
    check(calls > 0, "graphics_shadow_mesh calls > 0 (the route reached the first level's default meshes; calls=%s)" % mesh.get("calls", "<absent>"))
    check("model_calls" in mesh and mesh["model_calls"] == 0,
          "graphics_shadow_mesh model_calls=0 (no model body inside an owned bracket -- the s3 streak's arm; model_calls=%s)"
          % mesh.get("model_calls", "<absent>"))
    check(calls > 0 and mesh.get("replaced", -1) == calls,
          "graphics_shadow_mesh replaced == calls (every default mesh is the owner's; %s)" % (meta.get("graphics_shadow_mesh") or "<absent>"))
    shadow = counters(meta.get("graphics_shadow", ""))
    check("faults" in shadow and shadow["faults"] == 0, "graphics_shadow faults=0 (%s)" % shadow.get("faults", "<absent>"))
    for field in ("instances", "blocks", "groups"):
        check(shadow.get(field, 0) > 0, "graphics_shadow %s > 0 (owned meshes grouped and built; %s=%s)" % (field, field, shadow.get(field, "<absent>")))
    refusal_site = re.search(r"last_mesh_refusal=([^,]*)", meta.get("graphics_shadow", ""))
    check("refused_meshes" in shadow and shadow["refused_meshes"] == 0,
          "graphics_shadow refused_meshes=0 (every default mesh accepted; refused_meshes=%s last_mesh_refusal=%s)"
          % (shadow.get("refused_meshes", "<absent>"), refusal_site.group(1) if refusal_site else "<absent>"))
    scene = counters(meta.get("graphics_shadow_scene", ""))
    check(scene.get("published", 0) > 0, "graphics_shadow_scene published > 0 (passes drawn and composited; published=%s)" % scene.get("published", "<absent>"))
    check("published" in scene and "empty" in scene and "completed" in shadow and
          scene["published"] + scene["empty"] == shadow["completed"],
          "graphics_shadow_scene published + empty == graphics_shadow completed (a delivered pass draws or publishes nothing; %s vs completed=%s)"
          % (meta.get("graphics_shadow_scene") or "<absent>", shadow.get("completed", "<absent>")))
    check("empty" in shadow and "empty" in scene and 0 <= shadow["empty"] - scene["empty"] <= 2,
          "graphics_shadow empty == graphics_shadow_scene empty (+<=2 pending at the cap): a pass with no mesh publishes nothing (pass empty=%s scene empty=%s)"
          % (shadow.get("empty", "<absent>"), scene.get("empty", "<absent>")))
    refusals = counters(meta.get("graphics_refusals_shadows", ""))
    check("total" in refusals and refusals["total"] == 0,
          "graphics_refusals_shadows total=0 (no shadow refusal of any class; %s)" % (meta.get("graphics_refusals_shadows") or "<absent>"))
    check("faults" in refusals and refusals["faults"] == 0, "graphics_refusals_shadows faults=0 (%s)" % refusals.get("faults", "<absent>"))
    check(meta.get("video_scale") == str(VIDEO_SCALE),
          "video_scale=%d (the player's composition; video_scale=%s)" % (VIDEO_SCALE, meta.get("video_scale", "<absent>")))
    # allon_w2 (2026-10-11): the owned sky without its VU1 program residency sent ~10 % of
    # the route's kicks past the title's VU1 library (image never in the corpus) and play
    # mode ran away in memory (peak_mb 1654 / 3020 on this route; main: 553). The route's
    # resident peak is a row of this pin, and the library's miss share is bounded.
    footprint = counters(meta.get("footprint", ""))
    check("peak_mb" in footprint and footprint["peak_mb"] <= PEAK_MB_MAX,
          "footprint peak_mb <= %d (the level route stays bounded; peak_mb=%s last_mb=%s)"
          % (PEAK_MB_MAX, footprint.get("peak_mb", "<absent>"), footprint.get("last_mb", "<absent>")))
    classes = counters(meta.get("classes", ""))
    for family in ("skinned", "indexed-displace"):
        check(classes.get(family, 0) > 0,
              "classes %s > 0 (the %s VU1 family ran native on the route and its draws were lowered; %s=%s)"
              % (family, "skin" if family == "skinned" else "blend-shape", family, classes.get(family, "<absent>")))
    runs, misses = vu1_library(meta)
    check(runs > 0 and misses * VU1_MISS_SHARE_DENOMINATOR <= runs,
          "vu1_hle misses <= runs/%d (every kick's micro image is one the title's VU1 library knows; runs=%s misses=%s)"
          % (VU1_MISS_SHARE_DENOMINATOR, runs, misses))
    for key in PIN_FRAMES:
        checks.extend(judge_frame(key, frame_paths.get(key)))
        checks.extend(judge_frame(key, frame_paths.get(("p", key)), presented=True))
    return checks


class FrameKeeper(threading.Thread):
    """Deletes every dumped frame except the pin frames while the leg runs (a dumped
    frame is ~690 KB; a 7600-frame leg would otherwise write ~10 GB)."""

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
            if m.group(2) or int(m.group(1)) not in self.keep:
                try:
                    os.remove(os.path.join(self.directory, name))
                except OSError:
                    pass

    def run(self):
        while not self.stop.is_set():
            self.sweep()
            time.sleep(0.5)
        self.sweep()


def write_ppm(path, w, h, pixel_of, flip=True):
    """pixel_of(x, y_top_down) -> (r, g, b); written bottom-up like the target grab
    (flip) or top-down like the presented image."""
    with open(path, "wb") as f:
        f.write(b"P6\n%d %d\n255\n" % (w, h))
        order = range(h - 1, -1, -1) if flip else range(h)
        for y in order:
            f.write(bytes(c for x in range(w) for c in pixel_of(x, y)))


def fake_frame(path, footprint, streak, presented=False, crash=True):
    """A beach-like frame: sand (230, 205, 150) below logical row 150, sky above; optionally
    the shadow's darkening between the feet and/or a 32-px white column over Crash. The
    target grab is 512*VIDEO_SCALE wide, bottom-up; the presented image 640x448, top-down."""
    w, h = (640, 448) if presented else (512 * VIDEO_SCALE, 448 * VIDEO_SCALE)
    sx, sy = w / 512.0, h / 448.0

    def pixel(x, y):
        x, y = x / sx, y / sy
        if y < 150:
            return (120, 170, 230)
        r, g, b = 230, 205, 150
        if footprint and 240 <= x < 268 and 380 <= y < 406:
            r, g, b = 170, 150, 110
        if crash and 236 <= x < 272 and 286 <= y < 326:
            r, g, b = 125, 45, 4
        if streak and 236 <= x < 268 and 200 <= y < 420:
            r, g, b = 255, 255, 255
        return (r, g, b)
    write_ppm(path, w, h, pixel, flip=not presented)


# The tester's session 3 census (play_sessions/s3/run.meta, build 19:47:57): every default
# mesh refused at Shape and ran the model body inside the owned bracket; nothing accepted.
S3_META = (
    "frames=7600\nfinalized=1\ngraphics_output_faulted=0\ngraphics_shadow_selection=owned\n"
    "graphics_shadow_begin=calls=1883,replaced=1883,model_calls=0,interior=0\n"
    "graphics_shadow_mesh=calls=18728,replaced=0,model_calls=18728,interior=0\n"
    "graphics_shadow_end=calls=1883,replaced=1883,model_calls=0,interior=0\n"
    "graphics_shadow=instances=0,blocks=0,groups=0,completed=1881,faults=0,refused=0,refused_meshes=18728,"
    "last_mesh_refusal=check:4/result:2/object:0xbca9a0/geometry:0xbaa624/model_id:0xf37a152c/asset_id:0x0,restores=0\n"
    "graphics_refusals_shadows=total=18728,missing_resource=0,unsupported_state=18728,ordering=0,faults=0,"
    "first=frame:0/source_frame:0/n:1/producer:0x1be9f8/detail:516/reason:unsupported_state,"
    "last=frame:0/source_frame:0/n:18728/producer:0x1be9f8/detail:516/reason:unsupported_state,first_fault=none\n"
    "footprint=b60_mb=173,last_mb=464,peak_mb=553,final_mb=464,gsops_retained_bytes=0\nvu1_hle=8692000/109/4\nvu1_hle_worker=0/0/0\n"
    "classes=world-tristrip=9228304,skinned=2205830,particles-untex=0,indexed-displace=2846759,sprites-HUD=0,hle-other=22749,"
    "S9-shadow=0,raw-blit-fullscreen=0,raw-sprites-HUD=0,raw-other=44\n"
    "video_scale=2\npark=none\npark_pc=0x0\npark_detail=session_closed\n")
# The diag leg of the fixed pass on the same route (2026-10-09 21:43 build).
GOOD_META = (
    "frames=7600\nfinalized=1\ngraphics_output_faulted=0\ngraphics_shadow_selection=owned\n"
    "graphics_shadow_begin=calls=6310,replaced=6310,model_calls=0,interior=0\n"
    "graphics_shadow_mesh=calls=88000,replaced=88000,model_calls=0,interior=0\n"
    "graphics_shadow_end=calls=6310,replaced=6310,model_calls=0,interior=0\n"
    "graphics_shadow=instances=88000,blocks=15700,groups=264000,completed=6309,empty=1260,faults=0,refused=0,refused_meshes=0,"
    "last_mesh_refusal=none,restores=0\n"
    "graphics_shadow_assets=reads=643,converted=16,unselected=627,destroys=0,failed=0,refused=0,interior=0,loaded=16,released=0,rejected=0\n"
    "graphics_shadow_scene=published=5049,empty=1260\n"
    "graphics_refusals_shadows=total=0,missing_resource=0,unsupported_state=0,ordering=0,faults=0,first=none,last=none,first_fault=none\n"
    "footprint=b60_mb=173,last_mb=464,peak_mb=553,final_mb=464,gsops_retained_bytes=0\nvu1_hle=8692000/109/4\nvu1_hle_worker=0/0/0\n"
    "classes=world-tristrip=9228304,skinned=2205830,particles-untex=0,indexed-displace=2846759,sprites-HUD=0,hle-other=22749,"
    "S9-shadow=0,raw-blit-fullscreen=0,raw-sprites-HUD=0,raw-other=44\n"
    "video_scale=2\npark=none\npark_pc=0x0\npark_detail=frames_cap\n")
# The allon_w2 2a runaway on this route (level_play-with-2a/leg/run.meta, play): the owned sky
# without its VU1 program residency -- 1.15 M library misses, peak_mb 1654.
RUNAWAY_META = GOOD_META.replace("footprint=b60_mb=173,last_mb=464,peak_mb=553,final_mb=464",
                                 "footprint=b60_mb=171,last_mb=1654,peak_mb=1654,final_mb=1654").replace(
    "vu1_hle=8692000/109/4", "vu1_hle=8789269/1152634/4")
# The route never reached a default mesh (title/menu only): a route failure, not a pass.
NO_MESH_META = GOOD_META.replace("graphics_shadow_mesh=calls=88000,replaced=88000", "graphics_shadow_mesh=calls=0,replaced=0").replace(
    "instances=88000,blocks=15700,groups=264000", "instances=0,blocks=0,groups=0").replace(
    "graphics_shadow_scene=published=5049,empty=1260", "graphics_shadow_scene=published=0,empty=6309").replace("empty=1260,faults", "empty=6309,faults")


def selftest():
    import smoke_fake_runner as fake
    script = os.path.abspath(__file__)
    root = tempfile.mkdtemp(prefix="root_shadow_level_entry_selftest_")
    try:
        def frames_dir(name, footprint, streak, presented_footprint=None, keys=PIN_FRAMES, presented_keys=PIN_FRAMES,
                       crash=True):
            d = os.path.join(root, name)
            os.makedirs(d)
            for key in keys:
                fake_frame(os.path.join(d, "f%d.ppm" % key), footprint=footprint, streak=streak, crash=crash)
            for key in presented_keys:
                fake_frame(os.path.join(d, "p%d.ppm" % key), streak=streak, presented=True, crash=crash,
                           footprint=footprint if presented_footprint is None else presented_footprint)
            return d
        # The fake metas say video_scale=2 (the s4 composition): the fake target grabs are
        # 1024x896 and the cases pass --video-scale 2; good1 is the scale-1 arm's 512x448 grab.
        global VIDEO_SCALE
        VIDEO_SCALE = 1
        good1 = frames_dir("good1", True, False)
        VIDEO_SCALE = 2
        good = frames_dir("good", True, False)
        streak = frames_dir("streak", False, True)
        flat = frames_dir("flat", False, False)
        missing = frames_dir("missing", True, False, keys=PIN_FRAMES[:1])
        # The s4 shape: the target grab judged alone would pass, the presented image has no footprint.
        presented_flat = frames_dir("presented_flat", True, False, presented_footprint=False)
        no_presented = frames_dir("no_presented", True, False, presented_keys=())
        frames = ["--frames", "7600", "--video-scale", "2"]
        cases = [
            ("the s3 session: every mesh refused at Shape, the model body drew them", 0, S3_META, frames + ["--frames-from", streak], 1,
             "FAIL graphics_shadow_mesh model_calls=0"),
            ("the s3 session: nothing accepted", 0, S3_META, frames + ["--frames-from", streak], 1, "FAIL graphics_shadow instances > 0"),
            ("the s3 session: the refusal class is named", 0, S3_META, frames + ["--frames-from", streak], 1,
             "FAIL graphics_shadow refused_meshes=0 (every default mesh accepted; refused_meshes=18728 last_mesh_refusal=check:4/result:2"),
            ("the s3 session: the streak frame", 0, S3_META, frames + ["--frames-from", streak], 1,
             "FAIL frame f7000 no white vertical streak"),
            ("the s3 session: no footprint", 0, S3_META, frames + ["--frames-from", streak], 1,
             "FAIL frame f7000 dark footprint under Crash"),
            ("the route never reached a mesh is not a pass", 0, NO_MESH_META, frames + ["--frames-from", good], 1,
             "FAIL graphics_shadow_mesh calls > 0"),
            ("model arm is vacuous", 0, GOOD_META.replace("graphics_shadow_selection=owned", "graphics_shadow_selection=model"),
             frames + ["--frames-from", good], 1, "FAIL graphics_shadow_selection=owned"),
            ("one refused mesh names its class", 0, GOOD_META.replace("refused_meshes=0,last_mesh_refusal=none",
             "refused_meshes=1,last_mesh_refusal=check:9/result:1/object:0xbca9a0/geometry:0xbaa624/model_id:0xf37a152c/asset_id:0xf37a152c/screen:0x70003d80/camera:0x70003dc0"),
             frames + ["--frames-from", good], 1, "FAIL graphics_shadow refused_meshes=0 (every default mesh accepted; refused_meshes=1 last_mesh_refusal=check:9/result:1"),
            ("an empty pass that published", 0, GOOD_META.replace("graphics_shadow_scene=published=5049,empty=1260", "graphics_shadow_scene=published=6309,empty=0"),
             frames + ["--frames-from", good], 1, "FAIL graphics_shadow empty == graphics_shadow_scene empty"),
            ("a published pass unaccounted", 0, GOOD_META.replace("graphics_shadow_scene=published=5049,empty=1260", "graphics_shadow_scene=published=5000,empty=1260"),
             frames + ["--frames-from", good], 1, "FAIL graphics_shadow_scene published + empty == graphics_shadow completed"),
            ("a ledger fault", 0, GOOD_META.replace("ordering=0,faults=0", "ordering=0,faults=1"), frames + ["--frames-from", good], 1,
             "FAIL graphics_refusals_shadows faults=0"),
            ("a frame without the footprint", 0, GOOD_META, frames + ["--frames-from", flat], 1, "FAIL frame f7000 dark footprint under Crash"),
            ("a pin frame not dumped", 0, GOOD_META, frames + ["--frames-from", missing], 1, "FAIL frame f7500 dumped"),
            ("the s4 shape: the grab has the footprint, the PRESENTED frame has none", 0, GOOD_META,
             frames + ["--frames-from", presented_flat], 1, "FAIL presented frame p7000 dark footprint under Crash"),
            ("the presented frame not dumped", 0, GOOD_META, frames + ["--frames-from", no_presented], 1,
             "FAIL presented frame p7000 dumped"),
            ("the 2a runaway: the route's resident peak", 0, RUNAWAY_META, frames + ["--frames-from", good], 1,
             "FAIL footprint peak_mb <= 1000"),
            ("the 2a runaway: the VU1 library misses", 0, RUNAWAY_META, frames + ["--frames-from", good], 1,
             "FAIL vu1_hle misses <= runs/1000"),
            ("the shadow without Crash: no fur in the body box", 0, GOOD_META,
             frames + ["--frames-from", frames_dir("no_crash", True, False, crash=False)], 1,
             "FAIL frame f7000 Crash drawn over his footprint"),
            ("a route without the blend-shape family", 0, GOOD_META.replace("indexed-displace=2846759", "indexed-displace=0"),
             frames + ["--frames-from", good], 1, "FAIL classes indexed-displace > 0"),
            ("no footprint row", 0, GOOD_META.replace("footprint=b60_mb=173,last_mb=464,peak_mb=553,final_mb=464,gsops_retained_bytes=0\n", ""),
             frames + ["--frames-from", good], 1, "FAIL footprint peak_mb <= 1000"),
            ("the player's scale not composed", 0, GOOD_META.replace("video_scale=2", "video_scale=1"),
             frames + ["--frames-from", good], 1, "FAIL video_scale=2"),
            ("the scale-1 arm judges video_scale=1 through the same boxes", 0, GOOD_META.replace("video_scale=2", "video_scale=1"),
             ["--frames", "7600", "--video-scale", "1", "--frames-from", good1], 0, "RootShadowLevelEntry: ALL PASS"),
            ("the scale-1 arm: a frame of the other size is not a pass", 0, GOOD_META.replace("video_scale=2", "video_scale=1"),
             ["--frames", "7600", "--video-scale", "1", "--frames-from", good], 1, "FAIL frame f7000 is 512x448"),
            ("crash exit 134, unfinalized run.meta", 134, "frames=0\nfinalized=0\n", frames + ["--frames-from", good], 1, "FAIL bounded run exits 0"),
            ("runner kill 125 (stall)", 125, "frames=600\nfinalized=0\n", frames + ["--frames-from", good], 1, "RootShadowLevelEntry: FAIL"),
            ("exit 0, no run.meta", 0, None, frames + ["--frames-from", good], 1, "FAIL finalized=1"),
            ("good run", 0, GOOD_META, frames + ["--frames-from", good], 0, "RootShadowLevelEntry: ALL PASS"),
            ("good run, wrong frame cap", 0, GOOD_META, ["--frames", "7601", "--frames-from", good], 1, "FAIL frames=7601"),
        ]
        ok = fake.expect(cases, script)
    finally:
        shutil.rmtree(root, ignore_errors=True)
    print("root_shadow_level_entry selftest: %s" % ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", help="the Release nativeee")
    ap.add_argument("--game-files")
    ap.add_argument("--runner", help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=7600)
    ap.add_argument("--timeout", type=int, default=2400)
    ap.add_argument("--inputs", default=PADLOG, help="the route's padlog (PS2_PAD_SCRIPT grammar)")
    ap.add_argument("--set", action="append", default=[], dest="settings",
                    help="explicit Config selection passed to the binary as --set KEY=VAL (repeatable)")
    ap.add_argument("--frames-from", help="judge the pin frames from this directory instead of dumping (selftest/fake runner)")
    ap.add_argument("--keep", action="store_true", help="keep the harness output directory")
    ap.add_argument("--video-scale", type=int, default=DEFAULT_VIDEO_SCALE,
                    help="the player's video.scale for the leg and the judged video_scale= (default %d)" % DEFAULT_VIDEO_SCALE)
    ap.add_argument("--selftest", action="store_true", help="judge a fake runner (no game process)")
    args = ap.parse_args()
    global VIDEO_SCALE
    VIDEO_SCALE = args.video_scale
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
        print("SKIP: Release binary missing: %s" % args.binary)
        return 77
    if not os.path.isfile(args.inputs):
        print("FAIL: route padlog missing: %s" % args.inputs)
        return 1

    out_dir = tempfile.mkdtemp(prefix="root_shadow_level_entry_")
    log = out_dir + ".log"
    frames_dir = args.frames_from or os.path.join(out_dir, "frames")
    # A fresh, empty memory-card directory per run: the route's save-menu presses
    # were recorded against empty slots, and the autosave at the first level would
    # otherwise fill the shared default `memcards/` and move the next run's cursor.
    mc_dir = os.path.join(out_dir, "mc")
    os.makedirs(mc_dir, exist_ok=True)
    cmd = [args.runner, str(args.timeout), log, "--", args.binary,
           "--profile", "default", "--headless", "--harness-out", out_dir,
           "--frames", str(args.frames), "--game-files", os.path.abspath(args.game_files),
           "--inputs", os.path.abspath(args.inputs), "--set", "paths.mc_dir=" + mc_dir,
           "--set", "video.scale=%d" % VIDEO_SCALE]
    for field in DETERMINISTIC_FIELDS:
        cmd += ["--set", field]
    keeper = None
    if not args.frames_from:
        os.makedirs(frames_dir, exist_ok=True)
        cmd += ["--dump-frames", frames_dir]
        keeper = FrameKeeper(frames_dir, PIN_FRAMES)
        keeper.start()
    for setting in args.settings:
        cmd += ["--set", setting]
    print("run: " + " ".join(cmd))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
    env["RB_STALL_S"] = "300"
    proc = subprocess.run(cmd, env=env)
    if keeper is not None:
        keeper.stop.set()
        keeper.join()
    print("runner exit=%d" % proc.returncode)

    meta_path = os.path.join(out_dir, "run.meta")
    meta = parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    print("run.meta %s" % ("present" if os.path.isfile(meta_path) else "ABSENT (%s)" % meta_path))
    for key in ("frames", "finalized", "wall_ms", "park", "park_detail", "graphics_output_faulted", "video_scale",
                "footprint", "vu1_hle", "vu1_hle_worker", "classes",
                "graphics_shadow_selection", "graphics_shadow_begin", "graphics_shadow_mesh", "graphics_shadow_end",
                "graphics_shadow", "graphics_shadow_assets", "graphics_shadow_scene", "graphics_shadow_empty",
                "graphics_refusals_shadows"):
        print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
    frame_paths = {key: os.path.join(frames_dir, "f%d.ppm" % key) for key in PIN_FRAMES}
    frame_paths.update({("p", key): os.path.join(frames_dir, "p%d.ppm" % key) for key in PIN_FRAMES})

    failures = 0
    for ok, what in judge(proc.returncode, meta, args.frames, frame_paths):
        print("%s %s" % ("PASS" if ok else "FAIL", what))
        failures += 0 if ok else 1
    if failures:
        print("RootShadowLevelEntry: FAIL (%d) -- log %s, out %s" % (failures, log, out_dir))
        return 1
    print("RootShadowLevelEntry: ALL PASS")
    if not args.keep:
        shutil.rmtree(out_dir, ignore_errors=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
