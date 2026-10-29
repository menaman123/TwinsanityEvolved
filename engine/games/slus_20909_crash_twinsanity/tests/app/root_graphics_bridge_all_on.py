#!/usr/bin/env python3
"""RootGraphicsBridgeAllOn -- the five all-on/flip Graphics failures of the cutover
(shared-cause attribution)
as five deterministic headless coldboot legs at the compiled all-on defaults,
each 300 frames, each required to COMPLETE (run_leg finalized, park=none,
graphics_output_faulted=0) with the named family's owned work positive and its
refusal/fault rows at zero:

  frame63   the Movie presentation release at frame 63 (the tester's bundle):
            graphics_movie_transport failed=0 release_refused=0,
            graphics_owned_output failed=0, graphics_frame_send model_calls=0
  shadows   frame 113 "Shadow model load could not establish owned geometry":
            graphics_shadow_assets converted>0 refused=0 failed=0
  text      frame 119, the first owned text group fails delivery:
            graphics_text replaced>0 model_calls=0, graphics_producers failed=0
  strips    frame 118, the first owned DrawStrip non-Ok (flip 3):
            graphics_strip_output completed>0 failed=0, graphics_service failed_groups=0
  sprites   frame 60 texture refusals (flip 1): graphics_refusals_textures total=0,
            graphics_textures loaded>0 (the sprite census itself is the selection
            policy's -- recorded, not pinned)

Every leg is the same all-on composition; the five names are the five shapes the
one ordered-group lifecycle defect took. Exit 77 (SKIP) when the title files are
absent. Each leg also pins the lifecycle rows themselves: graphics_owned_output
failed=0, graphics_service ordering_errors=0 failed_groups=0,
graphics_refusals_{frame,strips,text,sprites,sky,shadows} faults=0.
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

ELF_REL = os.path.join("Crash_TwinSanity.rep", "SLUS_209.09")

LEGS = ("frame63", "shadows", "text", "strips", "sprites")


def parse_meta(path):
    meta = {}
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for line in f:
            line = line.rstrip("\n")
            if not line or "=" not in line:
                continue
            k, v = line.split("=", 1)
            meta[k.strip()] = v.strip()
    return meta


def fields(row):
    out = {}
    for item in row.split(","):
        if "=" in item:
            k, v = item.split("=", 1)
            out[k] = v
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary", required=True)
    ap.add_argument("--game-files", required=True)
    ap.add_argument("--runner", required=True, help="verify/run_bounded.sh")
    ap.add_argument("--frames", type=int, default=300)
    ap.add_argument("--timeout", type=int, default=50, help="per leg, seconds")
    ap.add_argument("--legs", default=",".join(LEGS))
    ap.add_argument("--keep", action="store_true")
    args = ap.parse_args()

    if not os.path.isfile(os.path.join(args.game_files, ELF_REL)):
        print("SKIP: title files absent under %s" % args.game_files)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1

    failures = 0

    def check(ok, what):
        nonlocal failures
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        if not ok:
            failures += 1

    def positive(meta, row, key, what):
        value = fields(meta.get(row, "")).get(key)
        check(value is not None and value.isdigit() and int(value) > 0, "%s %s.%s=%s > 0" % (what, row, key, value))

    def zero(meta, row, key, what):
        value = fields(meta.get(row, "")).get(key)
        check(value == "0", "%s %s.%s=%s == 0" % (what, row, key, value))

    for leg in [l for l in args.legs.split(",") if l]:
        out_dir = tempfile.mkdtemp(prefix="root_graphics_bridge_%s_" % leg)
        log = out_dir + ".log"
        cmd = [args.runner, str(args.timeout), log, "--", args.binary,
               "--profile", "deterministic", "--headless",
               "--harness-out", out_dir, "--frames", str(args.frames),
               "--game-files", os.path.abspath(args.game_files)]
        print("leg %s: %s" % (leg, " ".join(cmd)))
        env = dict(os.environ)
        env["RB_POLL_S"] = "1"
        env["RB_PROGRESS_FILE"] = os.path.join(out_dir, "frames.fhash")
        env["RB_STALL_S"] = str(args.timeout)
        proc = subprocess.run(cmd, env=env)
        print("runner exit=%d" % proc.returncode)
        check(proc.returncode == 0, "[%s] the all-on leg completes (runner exit 0)" % leg)
        meta_path = os.path.join(out_dir, "run.meta")
        if not os.path.isfile(meta_path):
            print("FAIL: [%s] no run.meta at %s (see %s)" % (leg, meta_path, log))
            failures += 1
            continue
        meta = parse_meta(meta_path)
        for key in ("frames", "finalized", "park", "park_detail", "graphics_output_faulted",
                    "graphics_owned_output", "graphics_service", "graphics_movie_transport",
                    "graphics_frame_send", "graphics_frame_output", "graphics_strip_output",
                    "graphics_text", "graphics_sprites", "graphics_textures", "graphics_producers",
                    "graphics_shadow_assets", "graphics_refusals_frame", "graphics_refusals_strips",
                    "graphics_refusals_text", "graphics_refusals_sprites", "graphics_refusals_textures",
                    "graphics_refusals_shadows"):
            print("run.meta %s=%s" % (key, meta.get(key, "<absent>")))
        tag = "[%s]" % leg
        check(meta.get("frames") == str(args.frames), "%s run.meta frames=%d" % (tag, args.frames))
        check(meta.get("finalized") == "1", "%s run.meta finalized=1" % tag)
        check(meta.get("park") == "none", "%s run.meta park=none" % tag)
        check(meta.get("graphics_output_faulted") == "0", "%s graphics_output_faulted=0" % tag)
        # The lifecycle rows every leg shares.
        zero(meta, "graphics_owned_output", "failed", tag)
        zero(meta, "graphics_service", "ordering_errors", tag)
        zero(meta, "graphics_service", "failed_groups", tag)
        for family in ("frame", "strips", "text", "sprites", "sky", "shadows", "textures"):
            zero(meta, "graphics_refusals_" + family, "faults", tag)
        if leg == "frame63":
            zero(meta, "graphics_movie_transport", "failed", tag)
            zero(meta, "graphics_movie_transport", "release_refused", tag)
            positive(meta, "graphics_movie_transport", "present_model_calls", tag)
            positive(meta, "graphics_frame_send", "replaced", tag)
            zero(meta, "graphics_frame_send", "model_calls", tag)
            positive(meta, "graphics_frame_output", "completed", tag)
            zero(meta, "graphics_frame_output", "retained_clears", tag)
            zero(meta, "graphics_refusals_frame", "total", tag)
        elif leg == "shadows":
            positive(meta, "graphics_shadow_assets", "converted", tag)
            zero(meta, "graphics_shadow_assets", "refused", tag)
            zero(meta, "graphics_shadow_assets", "failed", tag)
            zero(meta, "graphics_refusals_shadows", "total", tag)
        elif leg == "text":
            positive(meta, "graphics_text", "replaced", tag)
            zero(meta, "graphics_text", "model_calls", tag)
            zero(meta, "graphics_producers", "failed", tag)
            zero(meta, "graphics_producers", "refused_slots", tag)
            zero(meta, "graphics_refusals_text", "total", tag)
        elif leg == "strips":
            positive(meta, "graphics_strip_output", "completed", tag)
            zero(meta, "graphics_strip_output", "failed", tag)
            zero(meta, "graphics_strip_output", "retained", tag)
            positive(meta, "graphics_service", "owned_strips", tag)
            zero(meta, "graphics_service", "model_producers", tag)
            zero(meta, "graphics_refusals_strips", "total", tag)
        elif leg == "sprites":
            positive(meta, "graphics_textures", "loaded", tag)
            zero(meta, "graphics_textures", "failed", tag)
            zero(meta, "graphics_refusals_textures", "total", tag)
            zero(meta, "graphics_refusals_sprites", "total", tag)
            zero(meta, "graphics_sprites", "failed", tag)
        if not args.keep and failures == 0:
            shutil.rmtree(out_dir, ignore_errors=True)
            try:
                os.remove(log)
            except OSError:
                pass
        else:
            print("kept: %s (%s)" % (out_dir, log))

    print("RootGraphicsBridgeAllOn: %s" % ("ALL PASS" if failures == 0 else "%d FAILURES" % failures))
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
