#!/usr/bin/env python3
"""invincibility_judge.py -- THE THIRD AKU AKU MASK: does it make the character invincible, and what goes through it?

A third mask (a mask picked up with the shining one: 3 hit points, PickUpHealthCommand's fourth) starts 8 seconds of
invincibility (CharacterAgent::StartInvincibility: the part invulnerable, modeTicks 72000 clock units). While it runs a hit of
less than 50 takes nothing (CharacterAgent::Contact: InvincibleDamage) and one of 50 or more goes through -- every explosion's
is 100: retail's own rule, the same in the PAL and NTSC executables. game.invincible_to_explosions = on (off by default) spares
an invincible character explosions too, as the classic games' Aku Aku did (Platform::System::InvincibleToExplosions); the
speedrun mod keeps retail's.

Deterministic headless runs with the probe's [move] lines (TE_TRACE_MOVES=1: every change of the hit points, the invincibility
and the mode, with the last contact's kinds and damage) and the fixtures' test requests (TE_TEST_ACTIONS from each pad log's
"# test=" line):
 THE BEACH (fixtures/padlogs/invincible_beach.padlog: NEW GAME, the hit points set to 3, the east cliff's Aku Aku crate spun, a
 run into the nitro crate west of the start), --harness-out:
  A  the default        -> the crate's mask starts the invincibility (invulnerable 1, modeTicks `invincible_ticks`); the nitro's
                           explosion (kinds `explosion_kinds`, damage `explosion_damage`) takes the hit points to 0 while it runs,
                           the death and the checkpoint restart after it (retail's rule)
  B  game.invincible_to_explosions = on -> the same invincibility, at least one "an explosion spared" line, no hit point lost
                           and no death from the invincibility's start to its end; the frames and the state identical to A's up to
                           the explosion
  C  B's setting + --mod speedrun -> A's hit points exactly (the speedrun mod keeps retail's)
 JUNGLE BUNGLE (fixtures/padlogs/invincible_huba.padlog on the hub card warped to levels\\earth\\hub\\huba: LOAD GAME, the two
 Aku Aku crates' masks -- the second and the third --, a run into the crab):
  D  as recorded        -> the invincibility starts, and no hit point is lost to its end (the crab's contact taken by it)
  E  "hitpoints 2" asked between the crates -> the second crate's mask is the shining one again, no invincibility, and the crab's
                           contact (damage 1) takes a hit point: the route does meet the crab
A runner without the probe's health lines (no TE_TRACE_MOVES) or the hitpoints request is RED.

Usage: invincibility_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = a run failed.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "invincibility_pal.json")
HEALTH = re.compile(r"^\[move\] @(\d+) health hp (\d+) invulnerable (\d) mode (\d+) previous (\d+) modeTicks (-?\d+) "
                    r"contact kinds 0x([0-9A-Fa-f]+) damage (\d+)$")
SPARED = re.compile(r"invincible to explosions: an explosion spared @ boundary (\d+)")
TEST_LINE = re.compile(r"^# test=(.+?)(?:  \(.*)?$")
PLAYING, FADING_OUT, RESTARTING = 12, 21, 22


def test_actions(padlog):
    with open(padlog) as f:
        for line in f:
            m = TEST_LINE.match(line)
            if m:
                return m.group(1)
    return ""


def run(binary, iso, out, args_extra, env_extra, tag, harness):
    d = os.path.join(out, tag)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state"] + args_extra
    if harness:
        args += ["--harness-out", os.path.join(d, "harness")]
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_TRACE_MOVES"] = "1"
    if not harness:
        env["TE_GFX_NO_DRAW"] = "1"
    env.update(env_extra)
    print("invincibility_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("invincibility_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return d, rc, log


def health(log):
    """[(boundary, hp, invulnerable, modeTicks, kinds, damage)]"""
    out = []
    with open(log, errors="replace") as f:
        for line in f:
            m = HEALTH.match(line.rstrip("\n"))
            if m:
                out.append((int(m.group(1)), int(m.group(2)), int(m.group(3)), int(m.group(6)), int(m.group(7), 16), int(m.group(8))))
    return out


def spared(log):
    with open(log, errors="replace") as f:
        return [int(m.group(1)) for m in SPARED.finditer(f.read())]


def keyed(path):
    out = {}
    with open(path) as f:
        for line in f:
            p = line.strip().split(",")
            if len(p) >= 2 and p[0].isdigit():
                out[int(p[0])] = ",".join(p[1:])
    return out


def invincibility(rows, ticks):
    """the boundary the third mask's invincibility starts at (invulnerable with modeTicks `ticks`), and where it ends"""
    start = next((r[0] for r in rows if r[2] == 1 and r[3] == ticks), None)
    end = next((r[0] for r in rows if start is not None and r[0] > start and r[2] == 0), None)
    return start, end


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "invincibility_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    beach, huba = fx["beach"], fx["huba"]
    beach_padlog, huba_padlog = os.path.join(ROOT, beach["padlog"]), os.path.join(ROOT, huba["padlog"])
    ticks = fx["invincible_ticks"]

    # the beach: NEW GAME on empty cards
    option = os.path.join(a.out, "explosions_spared.ini")
    with open(option, "w") as f:
        f.write("game.invincible_to_explosions = on\n")
    legs = {}
    for tag, config, extra in (("A_default", "none", []), ("B_spared", option, []), ("C_spared_speedrun", option, ["--mod", "speedrun"])):
        mc = os.path.join(a.out, tag, "card", "mc")
        os.makedirs(mc)
        legs[tag] = run(a.bin, a.iso, a.out, ["--config", config, "--play-pad", beach_padlog, "--mc-dir", mc, "--stop-at",
                                              str(beach["stop_at"])] + extra, {"TE_TEST_ACTIONS": test_actions(beach_padlog)}, tag, True)

    # Jungle Bungle: the hub card (hub_entry.padlog to 2700, as tools/ci/level_entries.sh makes it) warped to huba
    hub = os.path.join(a.out, "hub", "mc")
    os.makedirs(hub)
    _, rc_hub, _ = run(a.bin, a.iso, a.out, ["--config", "none", "--play-pad", os.path.join(ROOT, "fixtures", "padlogs", "hub_entry.padlog"),
                                            "--mc-dir", hub, "--stop-at", "2700"], {}, "hub", False)
    actions = test_actions(huba_padlog)
    without = ";".join(item if not item.startswith(huba["second_crate_before"]) else "hitpoints 2@%d;%s" % (huba["between"], item)
                       for item in actions.split(";"))
    for tag, acts in (("D_third_mask", actions), ("E_shining_mask", without)):
        mc = os.path.join(a.out, tag, "card", "mc")
        os.makedirs(os.path.dirname(mc))
        subprocess.check_call([sys.executable, os.path.join(ROOT, "tools", "warp_save.py"), "--card", hub, "--chunk", huba["chunk"],
                               "--out", mc, "--place", "0xFFFF"], stdout=subprocess.DEVNULL)
        legs[tag] = run(a.bin, a.iso, a.out, ["--config", "none", "--play-pad", huba_padlog, "--mc-dir", mc, "--stop-at",
                                              str(huba["stop_at"])], {"TE_TEST_ACTIONS": acts}, tag, False)
    failed = {t: l[1] for t, l in legs.items() if l[1] != 0}
    if failed or rc_hub != 0:
        print("invincibility_judge: a run failed (%s, the hub card %d)" % (failed, rc_hub))
        return 2

    reasons = []
    ha, hb, hc = (health(legs[t][2]) for t in ("A_default", "B_spared", "C_spared_speedrun"))
    if not ha:
        reasons.append("no health lines: the runner has no TE_TRACE_MOVES probe")
    # A: the third mask's invincibility starts; the explosion goes through it
    start_a, end_a = invincibility(ha, ticks)
    kill = next((r for r in ha if start_a is not None and r[0] > start_a and r[1] == 0), None)
    if start_a is None:
        reasons.append("A: the crate's mask starts no invincibility (invulnerable with modeTicks %d)" % ticks)
    elif kill is None:
        reasons.append("A: the nitro's explosion takes no hit point through the invincibility (retail's rule)")
    else:
        if kill[2] != 1 or (end_a is not None and end_a <= kill[0]):
            reasons.append("A: the hit points reach 0 at %d outside the invincibility" % kill[0])
        if kill[4] & fx["explosion_kinds"] == 0 or kill[5] != fx["explosion_damage"]:
            reasons.append("A: the contact that took them is kinds 0x%X damage %d, not an explosion's %d" % (kill[4], kill[5], fx["explosion_damage"]))
        print("invincibility_judge: A the invincibility from %d; the explosion (kinds 0x%X, damage %d) takes the hit points to 0 at %d" % (
            start_a, kill[4], kill[5], kill[0]))
    ta = keyed(os.path.join(legs["A_default"][0], "harness", "state.trace"))
    if kill is not None and not any(b > kill[0] and s.startswith("%d," % FADING_OUT) for b, s in ta.items()):
        reasons.append("A: no fade-out after the death")
    # B: spared
    start_b, end_b = invincibility(hb, ticks)
    saved = spared(legs["B_spared"][2])
    lost_b = [r for r in hb if start_b is not None and r[0] > start_b and (end_b is None or r[0] < end_b) and r[1] < 3]
    if start_b is None or start_b != start_a:
        reasons.append("B: the invincibility starts at %s, A's at %s" % (start_b, start_a))
    if not saved:
        reasons.append("B: no 'an explosion spared' line")
    if lost_b:
        reasons.append("B: hit points lost while invincible: %s" % lost_b[:3])
    tb = keyed(os.path.join(legs["B_spared"][0], "harness", "state.trace"))
    if any(s.startswith("%d," % FADING_OUT) for b, s in tb.items() if start_b is not None and b > start_b):
        reasons.append("B: a fade-out (a death) after the invincibility's start")
    if kill is not None:
        fa = keyed(os.path.join(legs["A_default"][0], "harness", "frames.fhash"))
        fb = keyed(os.path.join(legs["B_spared"][0], "harness", "frames.fhash"))
        first = next((k for k in sorted(set(fa) & set(fb)) if fa[k] != fb[k]), None)
        if first is None or first < kill[0] - 1:
            reasons.append("A/B: the first differing frame is %s, the explosion at %d" % (first, kill[0]))
        print("invincibility_judge: B %d explosions spared (%s); A/B the first differing frame %s" % (len(saved), saved, first))
    # C: the speedrun mod keeps retail's
    if [r[1:] for r in hc if start_a is not None and r[0] >= start_a] != [r[1:] for r in ha if start_a is not None and r[0] >= start_a]:
        reasons.append("C: the speedrun mod's hit points after the invincibility's start are not A's (retail's)")
    if spared(legs["C_spared_speedrun"][2]):
        reasons.append("C: an explosion spared with the speedrun mod on")

    # D / E: the crab
    hd, he = health(legs["D_third_mask"][2]), health(legs["E_shining_mask"][2])
    start_d, end_d = invincibility(hd, ticks)
    lost_d = [r for r in hd if start_d is not None and r[0] > start_d and r[1] < 3]
    if start_d is None:
        reasons.append("D: the third mask starts no invincibility")
    if lost_d:
        reasons.append("D: hit points lost after the third mask: %s" % lost_d[:3])
    start_e, _ = invincibility(he, ticks)
    hurt_e = next((r for r in he if r[1] == 2 and r[5] == huba["crab_damage"] and r[2] == 1), None)
    if start_e is not None:
        reasons.append("E: an invincibility starts at %d with the shining mask asked back" % start_e)
    if hurt_e is None:
        reasons.append("E: the crab's contact takes no hit point (the route does not meet the crab)")
    else:
        print("invincibility_judge: D the invincibility from %s to %s, no hit point lost; E the crab's contact (kinds 0x%X, damage %d) "
              "takes one at %d" % (start_d, end_d, hurt_e[4], hurt_e[5], hurt_e[0]))

    if reasons:
        for r in reasons:
            print("invincibility_judge: " + r)
        print("invincibility_judge: RED (%d)" % len(reasons))
        return 1
    print("invincibility_judge: GREEN -- the third mask's invincibility starts (8 s) and the crab's contact takes nothing in it "
          "(a hit point without it); the nitro's explosion (100) goes through it by retail's rule, spared with "
          "game.invincible_to_explosions = on, retail's again with the speedrun mod")
    return 0


if __name__ == "__main__":
    sys.exit(main())
