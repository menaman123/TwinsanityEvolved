#!/usr/bin/env python3
"""root_mecha_bandicoot_meshes.py -- RootMechaBandicootMeshes: the owned mesh bucket after a savestate
RESTORE never mixes owned and model producers inside one frame's material list, and the Mecha-Bandicoot
fight resumed from the tester's slot0 presents a frame.

One play leg (`--harness-window`, the default profile, the state's own video.* selection, the s11 fight
padlog: Resume from the pause menu, run/spin/jump/strafe inside the arena) of FRAMES frames from the
state (`--state`; the tester's slot0 saved on baf15a38 at frame 37165, in the pause menu of the boss arena).

Census judge (run.meta of the leg):
  graphics_mesh_materials  mixed_frames=0            -- the admission gate: a frame is owned whole or model
                                                       whole (MeshProducer::FrameAdmits over the title's
                                                       material table); never both.
                           frames_admitted + frames_whole_model >= FRAMES - 2 (every frame decided)
                           no_receipt=0 receipt_stale=0 -- no restore artefact reaches owns_material
  graphics_refusals_meshes faults=0 and (total=0 OR mixed_frames=0 as above)
  graphics_meshes          faults=0;  graphics_output_faulted != 1
  frames=FRAMES finalized=1 park=none
Presented frame (<out>/w_final.ppm, the window image the leg ends on): exists, is not black (a
mean luminance above BLACK_FLOOR) -- the arena is on screen -- and, when `--model-binary` names a
frozen binary whose restore of the same state runs every mesh boundary on the model path (the
published baf15a38: no receipts, replaced=0), its presented frame of the same leg is compared over the
arena box within the 5-bit floor; the comparison is REPORTED (play legs from a windowed restore are not
frame-deterministic across binaries: the pace stall changes the title's own tick count) and judged
only when `--judge-model-frame` is given.

Red-before: a reference leg on baf15a38 read graphics_mesh_materials no_receipt=144000 next to
owned families replaced=0 -- every certifiable material of a restored image fell to the writer inside
the owned bracket, which is the mixture this pin forbids (a state WITH receipts and one uncertifiable
material would have mixed the two producers in one frame).

Usage: --binary --game-files --runner --state [--frames N] [--timeout S] [--keep DIR]
       [--model-binary B] [--judge-model-frame]; --selftest judges fake run.meta / frames.
SKIP (77): title files or the state absent, or no window (video=stub).
"""
import argparse
import os
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import root_shadow_level_entry as shadow  # noqa: E402  (parse_meta, counters, read_ppm, scaled)
import root_mesh_level_entry as mesh      # noqa: E402  (frame_match)

HERE = os.path.dirname(os.path.abspath(__file__))
PADLOG = os.path.join(HERE, "root_mecha_bandicoot_meshes.padlog")
ELF_REL = "Crash_TwinSanity.rep/SLUS_209.09"
FRAMES = 1200
# The state was saved with these; the restore refuses another clock/provider selection.
STATE_SETTINGS = ["video.anisotropy=16", "video.aspect=21:9", "video.texture_filter=point", "video.vsync=on"]
DEFAULT_STATE = os.path.join(os.path.expanduser("~"), "Library", "Application Support", "NativeEE", "logs",
                             "savestates", "slot0.ps2st")
ARENA_BOX = (40, 472, 150, 420)   # logical 512x448: the arena floor + rim
BLACK_FLOOR = 12.0
FLOOR_LEVELS = 8
MATCH_SHARE_MIN = 0.90


def judge_census(returncode, meta, frames):
    results = []

    def check(ok, what):
        results.append((bool(ok), what))
        return bool(ok)

    check(returncode == 0, "bounded run exits 0 (exit=%d)" % returncode)
    check(meta.get("finalized") == "1", "finalized=1")
    check(meta.get("frames") == str(frames), "frames=%d (frames=%s)" % (frames, meta.get("frames", "<absent>")))
    check(meta.get("park", "none") == "none", "no park (park=%s)" % meta.get("park", "none"))
    materials = shadow.counters(meta.get("graphics_mesh_materials", ""))
    meshes = shadow.counters(meta.get("graphics_meshes", ""))
    refusals = shadow.counters(meta.get("graphics_refusals_meshes", ""))
    check("mixed_frames" in materials, "census carries the admission counters (graphics_mesh_materials mixed_frames=)")
    check(materials.get("mixed_frames", 1) == 0, "no mixed frame (mixed_frames=%s)" % materials.get("mixed_frames"))
    decided = materials.get("frames_admitted", 0) + materials.get("frames_whole_model", 0)
    check(decided >= frames - 2, "every frame decided (admitted=%s whole_model=%s of %d)" %
          (materials.get("frames_admitted"), materials.get("frames_whole_model"), frames))
    check(materials.get("no_receipt", 1) == 0 and materials.get("receipt_stale", 1) == 0,
          "no restore artefact reaches owns_material (no_receipt=%s receipt_stale=%s)" %
          (materials.get("no_receipt"), materials.get("receipt_stale")))
    check(materials.get("table_unreadable", 1) == 0, "the title's material table read every frame (table_unreadable=%s)" %
          materials.get("table_unreadable"))
    check(refusals.get("faults", 1) == 0 and meshes.get("faults", 1) == 0,
          "no mesh fault (refusals faults=%s pass faults=%s)" % (refusals.get("faults"), meshes.get("faults")))
    check(meta.get("graphics_output_faulted", "0") != "1", "graphics_output_faulted != 1")
    check(materials.get("frames_admitted", 0) == 0 or meshes.get("flush_replaced", 0) > 0,
          "an admitted frame owns (admitted=%s flush_replaced=%s)" %
          (materials.get("frames_admitted"), meshes.get("flush_replaced")))
    return results


def judge_frame(path):
    results = []
    if not os.path.isfile(path):
        return [(False, "presented frame written (%s)" % path)]
    w, h, rows = shadow.read_ppm(path, flip=False)
    x0, x1, y0, y1 = shadow.scaled(ARENA_BOX, w / 512.0, h / 448.0)
    total = n = 0
    for y in range(y0, y1):
        row = rows[y]
        for x in range(x0, x1):
            i = x * 3
            total += (row[i] * 299 + row[i + 1] * 587 + row[i + 2] * 114) / 1000.0
            n += 1
    mean = total / float(max(1, n))
    results.append((mean > BLACK_FLOOR, "presented arena box is drawn (luminance mean %.1f > %.1f)" % (mean, BLACK_FLOOR)))
    return results


def run_leg(args, binary, out_dir, tag):
    leg = os.path.join(out_dir, tag)
    frames_dir = os.path.join(leg, "frames")
    mc_dir = os.path.join(out_dir, "mc_" + tag)
    os.makedirs(frames_dir)
    os.makedirs(mc_dir)
    log = os.path.join(out_dir, tag + ".log")
    cmd = [args.runner, str(args.timeout), log, "--", binary, "--profile", "default", "--harness-window",
           "--harness-out", leg, "--frames", str(args.frames), "--inputs", os.path.abspath(args.padlog),
           "--game-files", os.path.abspath(args.game_files), "--load-state", os.path.abspath(args.state),
           "--dump-frames", frames_dir, "--set", "paths.mc_dir=" + mc_dir,
           "--set", "paths.log_dir=" + os.path.join(leg, "logs")]
    for setting in STATE_SETTINGS:
        cmd += ["--set", setting]
    print("run[%s]: %s" % (tag, " ".join(cmd)))
    env = dict(os.environ)
    env["RB_POLL_S"] = "2"
    env["RB_PROGRESS_FILE"] = os.path.join(leg, "frames.fhash")
    env["RB_STALL_S"] = "120"
    proc = subprocess.run(cmd, env=env)
    meta_path = os.path.join(leg, "run.meta")
    meta = shadow.parse_meta(meta_path) if os.path.isfile(meta_path) else {}
    for key in ("frames", "finalized", "park", "video", "game_hz", "graphics_meshes", "graphics_mesh_materials",
                "graphics_refusals_meshes", "graphics_mesh_assets", "graphics_shadow", "graphics_output_faulted"):
        print("run.meta[%s] %s=%s" % (tag, key, meta.get(key, "<absent>")))
    return proc.returncode, meta, os.path.join(frames_dir, "w_final.ppm")


def selftest():
    failures = 0
    good = {"finalized": "1", "frames": "1200", "park": "none",
            "graphics_mesh_materials": "reads=0,no_receipt=0,receipt_stale=0,frames_admitted=0,frames_whole_model=1199,"
                                       "table_unreadable=0,mixed_frames=0",
            "graphics_meshes": "flush_replaced=0,faults=0", "graphics_refusals_meshes": "total=0,faults=0"}
    for ok, what in judge_census(0, good, 1200):
        if not ok:
            failures += 1
            print("selftest: good census failed: " + what)
    red = dict(good)
    red["graphics_mesh_materials"] = "reads=0,no_receipt=144000,receipt_stale=0,frames_admitted=1199,frames_whole_model=0,table_unreadable=0,mixed_frames=3"
    red["graphics_meshes"] = "flush_replaced=5,faults=0"
    bad = [what for ok, what in judge_census(0, red, 1200) if not ok]
    if len(bad) != 2:
        failures += 1
        print("selftest: red census should fail mixed + no_receipt, failed: %r" % bad)
    old = dict(good)
    old["graphics_mesh_materials"] = "reads=0,no_receipt=0"
    if all(ok for ok, _ in judge_census(0, old, 1200)):
        failures += 1
        print("selftest: a census without the admission counters must fail")
    tmp = tempfile.mkdtemp(prefix="root_mecha_selftest_")
    try:
        black = os.path.join(tmp, "black.ppm")
        lit = os.path.join(tmp, "lit.ppm")
        shadow.write_ppm(black, 512, 448, lambda x, y: (0, 0, 0), flip=False)
        shadow.write_ppm(lit, 512, 448, lambda x, y: (90, 110, 60), flip=False)
        if any(ok for ok, _ in judge_frame(black)):
            failures += 1
            print("selftest: a black presented frame must fail")
        if not all(ok for ok, _ in judge_frame(lit)):
            failures += 1
            print("selftest: a lit presented frame must pass")
        if any(ok for ok, _ in judge_frame(os.path.join(tmp, "absent.ppm"))):
            failures += 1
            print("selftest: an absent frame must fail")
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("RootMechaBandicootMeshesSelftest: %s" % ("PASS" if not failures else "FAIL (%d)" % failures))
    return 1 if failures else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--binary")
    ap.add_argument("--game-files")
    ap.add_argument("--runner")
    ap.add_argument("--state", default=DEFAULT_STATE)
    ap.add_argument("--padlog", default=PADLOG)
    ap.add_argument("--frames", type=int, default=FRAMES)
    ap.add_argument("--timeout", type=int, default=900)
    ap.add_argument("--keep", default="")
    ap.add_argument("--model-binary", default="")
    ap.add_argument("--judge-model-frame", action="store_true")
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
    if not os.path.isfile(args.state):
        print("SKIP: the Mecha-Bandicoot state is absent: %s" % args.state)
        return 77
    if not os.path.isfile(args.binary):
        print("FAIL: binary missing: %s" % args.binary)
        return 1
    out_dir = tempfile.mkdtemp(prefix="root_mecha_bandicoot_meshes_")
    rc, meta, frame = run_leg(args, args.binary, out_dir, "owned")
    if rc == 0 and meta.get("finalized") == "1" and meta.get("video") == "stub":
        print("SKIP: no window (video=stub): a play pin needs a display")
        shutil.rmtree(out_dir, ignore_errors=True)
        return 77
    results = judge_census(rc, meta, args.frames) + judge_frame(frame)
    if args.model_binary and os.path.isfile(args.model_binary):
        mrc, mmeta, mframe = run_leg(args, args.model_binary, out_dir, "model")
        match = mesh.frame_match(frame, mframe, ARENA_BOX, FLOOR_LEVELS) if os.path.isfile(mframe) and mrc == 0 else None
        text = "arena box within the 5-bit floor vs the model binary: %s" % (
            "share %.3f (differing %.3f)" % match if match else "no comparable frame")
        if args.judge_model_frame:
            results.append((bool(match) and match[0] >= MATCH_SHARE_MIN, text))
        else:
            print("  REPORT " + text)
    failures = 0
    for ok, what in results:
        print("  %s %s" % ("PASS" if ok else "FAIL", what))
        failures += not ok
    if args.keep:
        os.makedirs(args.keep, exist_ok=True)
        for tag in ("owned", "model"):
            leg = os.path.join(out_dir, tag)
            if os.path.isdir(leg):
                for name in ("run.meta",):
                    if os.path.isfile(os.path.join(leg, name)):
                        shutil.copy(os.path.join(leg, name), os.path.join(args.keep, tag + "_" + name))
                if os.path.isfile(os.path.join(leg, "frames", "w_final.ppm")):
                    shutil.copy(os.path.join(leg, "frames", "w_final.ppm"), os.path.join(args.keep, tag + "_w_final.ppm"))
    if failures == 0:
        shutil.rmtree(out_dir, ignore_errors=True)
    print("RootMechaBandicootMeshes: %s" % ("ALL PASS" if failures == 0 else "%d FAIL (artifacts: %s)" % (failures, out_dir)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
