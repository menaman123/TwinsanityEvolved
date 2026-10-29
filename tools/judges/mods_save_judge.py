#!/usr/bin/env python3
"""mods_save_judge.py -- with a mod ON, does the game's memory card writing stay off the VANILLA card?

Three deterministic headless runs of fixtures/padlogs/hub_entry.padlog (the title -> NEW GAME -> the save file created ->
the hub autosave) to `stop_at`, each on its own memory-card folder:
  A  no mod, an empty card folder            -> the vanilla card gets the new game's save (Bank0.bin...): the reference.
  B  `--mod <id>`, an empty card folder      -> GREEN needs: the vanilla folder holds NO file afterwards (never written) and the
                                                `-mods` folder holds the save (the game wrote its card there).
  C  `--mod <id>`, a COPY of A's vanilla card -> GREEN needs: every vanilla file's bytes identical before and after (sha256), the
                                                `-mods` folder present (seeded from the vanilla card, then written to).
  D  the LANGUAGE PACK (fixture "language_pack": mods/arabic) present and CHOSEN as the text language, its switch from an earlier
     version on in the settings file, an empty card folder -> GREEN needs: the vanilla folder holds the save, no `-mods` folder,
     the log names the vanilla card (a language pack is not a mod). A runner without text languages of its own (no
     --text-language) is run the way it chose the pack -- `--mod <pack> --language <pack>` -- and judged the same.
A runner that refuses `--mod` (a play binary without mods, exit 2) is RED: no mod can be enabled, nothing is namespaced.

RED on bin/twinsanityevolved.0f92911, GREEN on the mods build. 2026-10-27: D RED on 43e9989 (the pack switched on
sends the saves to the -mods card), GREEN on its change.

Usage: mods_save_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F]
Exit 0 = GREEN, 1 = RED, 2 = the run failed.
"""
import argparse
import hashlib
import json
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "mods_save_pal.json")


def run(binary, iso, out, fx, mc, extra, tag):
    d = os.path.join(out, tag)
    os.makedirs(d, exist_ok=True)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, fx["padlog"]), "--stop-at", str(fx["stop_at"]), "--mc-dir", mc,
            "--run-seconds", str(fx.get("run_seconds", 600))] + extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env["TE_GFX_NO_DRAW"] = "1"
    print("mods_save_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    print("mods_save_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return rc, log


def tree_hashes(folder):
    out = {}
    if not os.path.isdir(folder):
        return out
    for dirpath, _, files in os.walk(folder):
        for name in files:
            p = os.path.join(dirpath, name)
            with open(p, "rb") as f:
                out[os.path.relpath(p, folder)] = hashlib.sha256(f.read()).hexdigest()
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "mods_save_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    mod = fx["mod"]
    # A: the reference vanilla card
    mc_a = os.path.join(a.out, "mc_A")
    os.makedirs(mc_a)
    rc, _ = run(a.bin, a.iso, a.out, fx, mc_a, [], "A_vanilla")
    if rc != 0:
        print("mods_save_judge: run A failed (exit %d)" % rc)
        return 2
    a_hashes = tree_hashes(mc_a)
    if not any(fx["save_file"] in k for k in a_hashes):
        print("mods_save_judge: run A wrote no %s on the vanilla card (%s) -- the pad log did not reach the save" % (fx["save_file"], sorted(a_hashes)))
        return 2
    # B: a mod on, an empty card folder
    mc_b = os.path.join(a.out, "mc_B")
    os.makedirs(mc_b)
    rc, log_b = run(a.bin, a.iso, a.out, fx, mc_b, ["--mod", mod], "B_mod_empty")
    if rc == 2:
        print("mods_save_judge: RED -- the runner refuses --mod %s (no mod framework: nothing is namespaced)" % mod)
        return 1
    if rc != 0:
        print("mods_save_judge: run B failed (exit %d)" % rc)
        return 2
    b_vanilla, b_mods = tree_hashes(mc_b), tree_hashes(mc_b + "-mods")
    with open(log_b) as f:
        b_logged = "the -mods card" in f.read()
    # C: a mod on, over a copy of the vanilla card
    mc_c = os.path.join(a.out, "mc_C")
    shutil.copytree(mc_a, mc_c)
    before = tree_hashes(mc_c)
    rc, _ = run(a.bin, a.iso, a.out, fx, mc_c, ["--mod", mod], "C_mod_over_vanilla")
    if rc != 0:
        print("mods_save_judge: run C failed (exit %d)" % rc)
        return 2
    after, c_mods = tree_hashes(mc_c), tree_hashes(mc_c + "-mods")
    changed = sorted(k for k in set(before) | set(after) if before.get(k) != after.get(k))
    # D: the language pack chosen as the text language (not a mod): the vanilla card
    d_ok, d_line = True, "no language pack in the fixture"
    pack = fx.get("language_pack")
    if pack:
        mc_d = os.path.join(a.out, "mc_D")
        os.makedirs(mc_d)
        ini = os.path.join(a.out, "D_ps2r.ini")
        with open(ini, "w", newline="\n") as f:
            f.write("".join(x + "\n" for x in pack["settings"]))
        help_text = subprocess.run([a.bin, "--help"], capture_output=True, text=True, stdin=subprocess.DEVNULL, env=dict(os.environ, TE_GFX_HEADLESS="1")).stderr
        if "--text-language" in help_text:
            extra = ["--config", ini, "--mods-dir", os.path.join(ROOT, pack["mods_dir"]), "--text-language", pack["id"]]
        else:   # the runner's own way to choose the pack: a mod switched on, its language named
            extra = ["--config", ini, "--mods-dir", os.path.join(ROOT, pack["mods_dir"]), "--mod", pack["id"], "--language", pack["id"]]
        rc, log_d = run(a.bin, a.iso, a.out, fx, mc_d, extra, "D_language_pack")
        if rc != 0:
            print("mods_save_judge: run D failed (exit %d)" % rc)
            return 2
        with open(log_d, errors="replace") as f:
            d_text = f.read()
        d_vanilla, d_mods = tree_hashes(mc_d), tree_hashes(mc_d + "-mods")
        chosen = ("-> text %s by name" % pack["id"]) in d_text or ("requested '%s'" % pack["id"]) in d_text
        d_ok = (any(fx["save_file"] in k for k in d_vanilla) and not d_mods and not os.path.isdir(mc_d + "-mods") and "the -mods card" not in d_text)
        d_line = "D (the %s pack chosen%s): vanilla files %d (%s), -mods %s" % (pack["id"], "" if chosen else " -- NOT reported chosen", len(d_vanilla),
                                                                                "save present" if any(fx["save_file"] in k for k in d_vanilla) else "NO save",
                                                                                ("%d files" % len(d_mods)) if os.path.isdir(mc_d + "-mods") else "absent")
        d_ok = d_ok and chosen
        print("mods_save_judge: %s" % d_line)
    print("mods_save_judge: A vanilla files %d; B vanilla files %d, -mods files %d (%s), log names the -mods card: %s; "
          "C vanilla files changed %d of %d (%s), -mods files %d"
          % (len(a_hashes), len(b_vanilla), len(b_mods), "save present" if any(fx["save_file"] in k for k in b_mods) else "NO save",
             b_logged, len(changed), len(before), changed[:5], len(c_mods)))
    green = (len(b_vanilla) == 0 and any(fx["save_file"] in k for k in b_mods) and b_logged and not changed and len(c_mods) > 0)
    if green and d_ok:
        print("mods_save_judge: GREEN -- with %s on, the card writes went to the -mods card and the vanilla card's bytes never changed; "
              "with the %s language pack chosen they stayed on the vanilla card" % (mod, pack["id"] if pack else "(no)"))
        return 0
    print("mods_save_judge: RED -- %s" % ("the vanilla card was written with a mod on" if (b_vanilla or changed) else
                                       "no save reached the -mods card" if not b_mods else
                                       "a language pack sent the saves off the vanilla card (%s)" % d_line if not d_ok else "see the figures"))
    return 1


if __name__ == "__main__":
    sys.exit(main())
