#!/usr/bin/env python3
"""texture_pipeline_judge.py -- the game's textures made ahead of their use (TE_GFX_EAGER_TEXTURES, the default on every
platform): in a cutscene that reads its next area while it plays, no texture is decoded on the game thread and none is
waited for there, and every picture, sound and state is the synchronous path's.

The synchronous path (TE_GFX_EAGER_TEXTURES=0) decodes a texture on the game thread at the game's ReadTexture and makes it on
the drawing thread with the game thread waiting; the drawing thread takes that job between two pictures, so while it is
behind -- a GPU-bound picture queued after the one it draws, at the internal scale's 4x -- each texture costs the game thread
a picture's drawing: 20-58 ms ticks in the level transition's camera cuts on an M4 at 4x. The default reserves the texture's
id at the call, decodes it on a builder and makes it on the drawing thread between pictures or at its first draw.

Per cutscene of the fixture (fixtures/judges/texture_pipeline_pal.json: cutscenes of fixtures/judges/cutscene_skip_pal.json,
reached as tools/judges/cutscene_skip_judge.py reaches them -- the warped card, the pad log, TE_CUTSCENE_START -- and watched),
four deterministic headless legs with --harness-out and TE_LOAD_CENSUS=1 (the load census' window lines every 30 presents):
  default, sync (TE_GFX_EAGER_TEXTURES=0), and both again with the drawing thread on (TE_GFX_RENDER_THREAD=1, a player's
  threading).
Judged, per cutscene:
  the path      the default legs log "textures: decoded ahead", the sync legs "decoded and made at their load"
  ahead         the default legs: no texture took the synchronous path (the load census' "texture decode: 0 textures ... on the
                game thread" in every window), at least min_ahead decoded ahead ("... + N ... off it"), "failed 0" in every window
                of the ahead census
  not vacuous   the sync legs decode at least min_ahead textures on the game thread (the leg does read textures)
  identity      frames.fhash, audio.ahash, state.trace and frames.pdesc of default == sync and of the two drawing-thread legs;
                frames.fhash, audio.ahash and state.trace of the drawing-thread legs == sync's (the runner draws the pictures
                handed over before the harness closes, so they record every picture; frames.pdesc's means are computed on the
                thread that presents, under the game thread's rounding toward zero when there is no drawing thread -- their
                last digit differs between the two threadings, never within one)
Usage: texture_pipeline_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--hub-card DIR]
Exit 0 = GREEN, 1 = RED, 2 = a run failed.
"""
import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "texture_pipeline_pal.json")
sys.path.insert(0, os.path.join(ROOT, "tools", "cutscenes"))
import verify  # noqa: E402

STREAMS = ("frames.fhash", "audio.ahash", "state.trace", "frames.pdesc")
DECODE = re.compile(r"texture decode: (\d+) textures, \d+ KB, [\d.]+ ms this window on the game thread \+ (\d+), \d+ KB")
AHEAD = re.compile(r"\[te\.load\] textures ahead of their use .*; failed (\d+);")
LEGS = (("default", {}), ("sync", {"TE_GFX_EAGER_TEXTURES": "0"}),
        ("default_thread", {"TE_GFX_RENDER_THREAD": "1"}), ("sync_thread", {"TE_GFX_EAGER_TEXTURES": "0", "TE_GFX_RENDER_THREAD": "1"}))


def leg(binary, iso, out, hub, item, env_extra):
    os.makedirs(out, exist_ok=True)
    stop = item.get("stop", 4800)
    plan = os.path.join(ROOT, item["plan"]) if item.get("plan") else None
    entry = os.path.join(ROOT, item.get("padlog", "fixtures/padlogs/entries/school_sch_hub_sch_hub.padlog"))
    mc = verify.card(out, hub, item["chunk"], item.get("character", "crash"), "w", item)
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--config", "none", "--trace-state",
            "--mc-dir", mc] + verify.leg_args(entry, out, "W", None, plan, 300) + ["--stop-at", str(stop), "--harness-out",
                                                                                   os.path.join(out, "h"), "--run-seconds", "900"]
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_LOAD_CENSUS="1", TE_GFX_SUMMARY_EVERY="30")
    for k in ("TE_CUTSCENE_START", "TE_TEST_ACTIONS", "TE_GFX_NO_DRAW", "TE_GFX_EAGER_TEXTURES", "TE_GFX_RENDER_THREAD",
              "TE_GFX_DUMP_TEXTURES"):
        env.pop(k, None)
    if item.get("start"):
        env["TE_CUTSCENE_START"] = item["start"]
    if item.get("test"):
        env["TE_TEST_ACTIONS"] = item["test"]
    env.update(env_extra)
    log = os.path.join(out, "run.log")
    with open(log, "w") as f:
        rc = subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, env=env, cwd=ROOT).returncode
    shutil.rmtree(mc, ignore_errors=True)
    text = open(log, errors="replace").read()
    r = {"exit": rc, "streams": {}}
    for s in STREAMS:
        p = os.path.join(out, "h", s)
        r["streams"][s] = hashlib.sha256(open(p, "rb").read()).hexdigest() if os.path.exists(p) else None
    m = re.search(r"\[te\.gfx\] textures: ([^(]*)\(TE_GFX_EAGER_TEXTURES\)", text)
    r["path"] = m.group(1).strip() if m else ""
    on_game = off_game = 0
    for d in DECODE.finditer(text):
        on_game += int(d.group(1))
        off_game += int(d.group(2))
    r["decoded_on_game_thread"], r["decoded_off_it"] = on_game, off_game
    r["ahead_failed"] = sum(int(x.group(1)) for x in AHEAD.finditer(text))
    r["ahead_lines"] = len(AHEAD.findall(text))
    shutil.rmtree(os.path.join(out, "h"), ignore_errors=True)
    return r


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "texture_pipeline_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--hub-card", default="")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    cards = {c["name"]: c for c in json.load(open(os.path.join(ROOT, fx["cutscenes_from"])))["cutscenes"]}
    os.makedirs(a.out, exist_ok=True)
    hub = a.hub_card
    if not hub:
        hub = os.path.join(a.out, "mc_hub")
        shutil.rmtree(hub, ignore_errors=True)
        os.makedirs(hub)
        verify.make_hub_card(os.path.abspath(a.bin), a.iso, hub, os.path.join(a.out, "hub.log"))
    red = []
    for c in fx["cutscenes"]:
        item = cards[c["name"]]
        res = {}
        for tag, env in LEGS:
            res[tag] = leg(os.path.abspath(a.bin), a.iso, os.path.join(a.out, c["name"], tag), hub, item, env)
            r = res[tag]
            print("texture_pipeline_judge: %s %-15s exit %d, %s; decoded on the game thread %d, off it %d; ahead lines %d, failed %d" % (
                c["name"], tag, r["exit"], r["path"], r["decoded_on_game_thread"], r["decoded_off_it"], r["ahead_lines"], r["ahead_failed"]),
                flush=True)
            if r["exit"] != 0 or not all(r["streams"].values()):
                print("texture_pipeline_judge: a run failed (%s %s)" % (c["name"], tag))
                sys.exit(2)
        why = []
        for tag in ("default", "default_thread"):
            r = res[tag]
            if not r["path"].startswith("decoded ahead"):
                why.append("%s: the path is '%s'" % (tag, r["path"]))
            if r["decoded_on_game_thread"] != 0:
                why.append("%s: %d textures decoded on the game thread" % (tag, r["decoded_on_game_thread"]))
            if r["decoded_off_it"] < c["min_ahead"]:
                why.append("%s: %d textures decoded off the game thread (< %d)" % (tag, r["decoded_off_it"], c["min_ahead"]))
            if r["ahead_lines"] == 0 or r["ahead_failed"] != 0:
                why.append("%s: the ahead census %d lines, failed %d" % (tag, r["ahead_lines"], r["ahead_failed"]))
        for tag in ("sync", "sync_thread"):
            r = res[tag]
            if not r["path"].startswith("decoded and made at their load"):
                why.append("%s: the path is '%s'" % (tag, r["path"]))
            if r["decoded_on_game_thread"] < c["min_ahead"]:
                why.append("%s: only %d textures decoded on the game thread (< %d): the leg reads no textures" % (
                    tag, r["decoded_on_game_thread"], c["min_ahead"]))
        for x, y, streams in (("default", "sync", STREAMS), ("default_thread", "sync_thread", STREAMS),
                              ("default_thread", "sync", STREAMS[:3])):
            for s in streams:
                if res[x]["streams"][s] != res[y]["streams"][s]:
                    why.append("%s differs between %s and %s" % (s, x, y))
        print("texture_pipeline_judge: %s %s" % (c["name"], "GREEN" if not why else "RED: " + "; ".join(why)))
        if why:
            red.append(c["name"])
    print("texture_pipeline_judge: verdict %s" % ("GREEN (%d cutscenes)" % len(fx["cutscenes"]) if not red else "RED (%s)" % ", ".join(red)))
    sys.exit(1 if red else 0)


if __name__ == "__main__":
    main()
