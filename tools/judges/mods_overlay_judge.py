#!/usr/bin/env python3
"""mods_overlay_judge.py -- do mods placed in the mods folder reach the game through the disc layer, and does nothing change
when none is on?

The mods are made at run time from the disc by tools/mods/make_fixture_overlay.py (never committed; deleted afterwards):
`textbank` (the menu line "graphic options" -> "graphic optionz" in the text bank the OPTIONS page shows), `levelchunk` (a
harmless byte of the hub's beach.rm2: its root section version), `order_a` / `order_b` (the same line two ways). Every run is
deterministic and headless (fixtures/judges/mods_overlay_pal.json):

  (i)   TEXT   the OPTIONS route, TE_GFX_DIAG_TEXT=1 (every drawn text logged with its string): vanilla draws the original line
               (the route reaches the page), --mod textbank draws the changed one and never the original.
  (ii)  CHUNK  the hub entry route to 4200 with --mod levelchunk, TE_STREAM_TRACE=1: the run reaches the end with Playing held,
               an archive read covers the changed byte, the census row mods=<layers>:<overrides>:<hits> counts hits, and (the
               change being harmless) the pictures, sound and states equal the vanilla hub run's.
  (iii) ORDER  the OPTIONS route with both order mods on, mods.order = a,b then b,a (a settings file each): the later one's line
               is drawn, and order_b's second change shows both ways.
  (iv)  NONE   the determinism gate's two legs (title_intro_idle to 3300, hub_entry to 4200) without a mods folder and with the
               fixture mods in the folder but none on: frames.fhash / audio.ahash / state.trace byte-identical (and to
               --ref-bin's run when given).

A runner without the mods folder (it refuses --mods-dir: exit 2) is RED: no mod can reach the disc.

RED on the base (234718f), GREEN on the disc overlay build.

Usage: mods_overlay_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--ref-bin BASE_BIN] [--keep]
Writes OUT/report.json. Exit 0 = GREEN, 1 = RED, 2 = the setup failed (the disc, the generator, a vanilla run).
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "mods_overlay_pal.json")
STRING = re.compile(r"\[te\.gfx\.diag\] present \d+ text \d+ quads .* string '(.*)'$")
STATE = re.compile(r"game state @ boundary (\d+): (\w+) \((\d+)\)")
CENSUS = re.compile(r"^\[twinsanityevolved\] mods=(\d+):(\d+):(\d+)\s*$", re.M)
OPEN = re.compile(r'\[stream\] open #(\d+) "([^"]+)"')
READ = re.compile(r"\[stream\] ch\d+ read #(\d+) \+0x([0-9A-Fa-f]+) (\d+) bytes")


def run(binary, iso, out, tag, padlog, stop_at, extra, env_extra, run_seconds, harness=False):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, padlog), "--stop-at", str(stop_at), "--mc-dir", mc, "--run-seconds", str(run_seconds)]
    if harness:
        args += ["--harness-out", os.path.join(d, "harness")]
    args += extra
    env = dict(os.environ)
    env["TE_GFX_HEADLESS"] = "1"
    env.update(env_extra)
    print("mods_overlay_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    with open(log, errors="replace") as f:
        text = f.read()
    print("mods_overlay_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return {"rc": rc, "log": log, "text": text, "dir": d}


def strings(text):
    counts = {}
    for line in text.splitlines():
        m = STRING.match(line)
        if m:
            s = m.group(1).lower()
            counts[s] = counts.get(s, 0) + 1
    return counts


def census(text):
    rows = CENSUS.findall(text)
    return tuple(int(v) for v in rows[-1]) if rows else None


def same_files(dir_a, dir_b, files):
    out = {}
    for name in files:
        pa, pb = os.path.join(dir_a, name), os.path.join(dir_b, name)
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            out[name] = "missing"
            continue
        with open(pa, "rb") as fa, open(pb, "rb") as fb:
            out[name] = "identical" if fa.read() == fb.read() else "DIFFERS"
    return out


def refused(r):
    return r["rc"] == 2 and ("usage:" in r["text"] or "--mods-dir" in r["text"] or "--mod " in r["text"])


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "mods_overlay_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--ref-bin", default="", help="a reference build (the base): the no-mod legs must equal its streams")
    ap.add_argument("--keep", action="store_true", help="keep the generated mod folders and the runs")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    binary = os.path.abspath(a.bin)
    iso = os.path.abspath(a.iso)
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    report = {"bin": binary, "iso": iso, "fixture": os.path.relpath(a.fixture, ROOT), "checks": {}, "verdict": None}

    def finish(code, line):
        report["verdict"] = line
        with open(os.path.join(a.out, "report.json"), "w") as f:
            json.dump(report, f, indent=2)
        print("mods_overlay_judge: %s" % line)
        if not a.keep:
            shutil.rmtree(os.path.join(a.out, "mods"), ignore_errors=True)
            for name in os.listdir(a.out):
                p = os.path.join(a.out, name)
                if os.path.isdir(p):
                    shutil.rmtree(p, ignore_errors=True)
        return code

    # --- the mods, from the disc
    mods = os.path.join(a.out, "mods")
    gen = subprocess.run([sys.executable, os.path.join(ROOT, fx["generator"]), "--iso", iso, "--out", mods],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    print(gen.stdout, end="")
    if gen.returncode != 0:
        return finish(2, "SETUP FAILED -- the fixture generator: %s" % gen.stdout.strip().splitlines()[-1:])
    with open(os.path.join(mods, "report.json")) as f:
        made = json.load(f)["mods"]
    os.remove(os.path.join(mods, "report.json"))   # not a mod folder
    text_fx, chunk_fx, order_fx, det_fx = fx["text"], fx["chunk"], fx["order"], fx["det"]
    original = text_fx["string"].lower()
    changed = made["textbank"]["to"].lower()
    red = []

    # --- (i) TEXT
    v = run(binary, iso, a.out, "text_vanilla", text_fx["padlog"], text_fx["stop_at"], ["--config", "none"],
            {"TE_GFX_DIAG_TEXT": "1"}, text_fx["run_seconds"])
    if v["rc"] != 0:
        return finish(2, "SETUP FAILED -- the vanilla OPTIONS run exited %d" % v["rc"])
    vs = strings(v["text"])
    if vs.get(original, 0) < text_fx["min_draws"]:
        return finish(2, "SETUP FAILED -- the vanilla run drew '%s' %d times (< %d): the route did not reach the OPTIONS page" %
                      (original, vs.get(original, 0), text_fx["min_draws"]))
    t = run(binary, iso, a.out, "text_mod", text_fx["padlog"], text_fx["stop_at"],
            ["--config", "none", "--mods-dir", mods, "--mod", "textbank"], {"TE_GFX_DIAG_TEXT": "1"}, text_fx["run_seconds"])
    ts = strings(t["text"])
    check = {"vanilla_original_draws": vs.get(original, 0), "mod_changed_draws": ts.get(changed, 0),
             "mod_original_draws": ts.get(original, 0), "rc": t["rc"], "census": census(t["text"])}
    if refused(t):
        check["result"] = "RED: the runner refuses --mods-dir / --mod (no mods folder)"
    elif t["rc"] != 0:
        check["result"] = "RED: the modded run exited %d" % t["rc"]
    elif ts.get(changed, 0) >= text_fx["min_draws"] and ts.get(original, 0) == 0:
        check["result"] = "GREEN"
    else:
        check["result"] = "RED: '%s' drawn %d times, '%s' %d times" % (changed, ts.get(changed, 0), original, ts.get(original, 0))
    report["checks"]["text"] = check
    if check["result"] != "GREEN":
        red.append("text: " + check["result"])

    # --- (iv) NONE first: its vanilla hub run is (ii)'s reference
    legs = {}
    for leg in det_fx["legs"]:
        n = run(binary, iso, a.out, "none_%s_nofolder" % leg["name"], leg["padlog"], leg["stop_at"], ["--config", "none"], {},
                det_fx["run_seconds"], harness=True)
        if n["rc"] != 0:
            return finish(2, "SETUP FAILED -- the no-mod %s run exited %d" % (leg["name"], n["rc"]))
        fdir = run(binary, iso, a.out, "none_%s_folder_off" % leg["name"], leg["padlog"], leg["stop_at"],
                   ["--config", "none", "--mods-dir", mods], {}, det_fx["run_seconds"], harness=True)
        entry = {"nofolder_rc": n["rc"], "folder_off_rc": fdir["rc"]}
        if refused(fdir):
            entry["result"] = "RED: the runner refuses --mods-dir"
        elif fdir["rc"] != 0:
            entry["result"] = "RED: the run with the mods folder exited %d" % fdir["rc"]
        else:
            entry["folder_off_vs_nofolder"] = same_files(os.path.join(n["dir"], "harness"), os.path.join(fdir["dir"], "harness"), det_fx["files"])
            entry["census_folder_off"] = census(fdir["text"])
            ok = all(vv == "identical" for vv in entry["folder_off_vs_nofolder"].values()) and entry["census_folder_off"] == (0, 0, 0)
            if a.ref_bin:
                ref = run(os.path.abspath(a.ref_bin), iso, a.out, "none_%s_ref" % leg["name"], leg["padlog"], leg["stop_at"],
                          ["--config", "none"], {}, det_fx["run_seconds"], harness=True)
                entry["vs_ref_bin"] = same_files(os.path.join(ref["dir"], "harness"), os.path.join(n["dir"], "harness"), det_fx["files"])
                ok = ok and ref["rc"] == 0 and all(vv == "identical" for vv in entry["vs_ref_bin"].values())
            entry["result"] = "GREEN" if ok else "RED: a no-mod stream differs (%s)" % {k: entry.get(k) for k in ("folder_off_vs_nofolder", "vs_ref_bin")}
        legs[leg["name"]] = (entry, n)
        report["checks"]["none_" + leg["name"]] = entry
        if entry["result"] != "GREEN":
            red.append("none %s: %s" % (leg["name"], entry["result"]))

    # --- (ii) CHUNK
    hub_vanilla = legs.get("hub_entry", (None, None))[1]
    c = run(binary, iso, a.out, "chunk_mod", chunk_fx["padlog"], chunk_fx["stop_at"],
            ["--config", "none", "--mods-dir", mods, "--mod", "levelchunk"], {"TE_STREAM_TRACE": "1"}, chunk_fx["run_seconds"], harness=True)
    check = {"rc": c["rc"], "chunk": made["levelchunk"]["chunk"], "archive_offset": made["levelchunk"]["archive_offset"],
             "version": "%d -> %d" % (made["levelchunk"]["version_from"], made["levelchunk"]["version_to"])}
    if refused(c):
        check["result"] = "RED: the runner refuses --mods-dir / --mod (no mods folder)"
    elif c["rc"] != 0:
        check["result"] = "RED: the modded hub run exited %d" % c["rc"]
    else:
        states = [(int(b), name) for b, name, _ in STATE.findall(c["text"])]
        reached = "--stop-at %d reached" % chunk_fx["stop_at"] in c["text"]
        final = states[-1][1] if states else None
        playing_from = states[-1][0] if states and final == chunk_fx["final_state"] else None
        bd_files = {int(m.group(1)) for m in OPEN.finditer(c["text"]) if m.group(2).upper().replace("/", "\\").endswith("CRASH.BD")}
        at = made["levelchunk"]["archive_offset"]
        covered = 0
        for m in READ.finditer(c["text"]):
            if int(m.group(1)) in bd_files:
                off, n_bytes = int(m.group(2), 16), int(m.group(3))
                covered += 1 if off <= at < off + n_bytes else 0
        row = census(c["text"])
        check.update({"reached_stop": reached, "final_state": final, "playing_from": playing_from, "reads_over_the_byte": covered,
                      "census": row})
        ok = reached and final == chunk_fx["final_state"] and playing_from is not None and playing_from <= chunk_fx["playing_by"] \
            and covered > 0 and row is not None and row[0] == 1 and row[2] >= chunk_fx["min_hits"]
        if chunk_fx.get("frames_identical_to_vanilla") and hub_vanilla is not None:
            check["vs_vanilla"] = same_files(os.path.join(hub_vanilla["dir"], "harness"), os.path.join(c["dir"], "harness"), det_fx["files"])
            ok = ok and all(vv == "identical" for vv in check["vs_vanilla"].values())
        check["result"] = "GREEN" if ok else "RED: %s" % {k: check.get(k) for k in ("reached_stop", "final_state", "playing_from",
                                                                                    "reads_over_the_byte", "census", "vs_vanilla")}
    report["checks"]["chunk"] = check
    if check["result"] != "GREEN":
        red.append("chunk: " + check["result"])

    # --- (iii) ORDER
    want = made["order"]
    check = {}
    for order, wins in (("order_a,order_b", want["b_wins"]), ("order_b,order_a", want["a_wins"])):
        ini = os.path.join(a.out, "order_%s.ini" % order.replace(",", "_then_"))
        with open(ini, "w") as f:
            f.write("mods.order_a.enabled = on\nmods.order_b.enabled = on\nmods.order = %s\npaths.mods = %s\n" % (order, mods))
        o = run(binary, iso, a.out, "order_" + order.replace(",", "_then_"), order_fx["padlog"], order_fx["stop_at"],
                ["--config", ini, "--mods-dir", mods], {"TE_GFX_DIAG_TEXT": "1"}, order_fx["run_seconds"])
        os_ = strings(o["text"])
        losers = [s for s in (want["a_wins"], want["b_wins"], original) if s != wins]
        entry = {"rc": o["rc"], "wins_draws": os_.get(wins.lower(), 0), "other_draws": {s: os_.get(s.lower(), 0) for s in losers},
                 "b_also_draws": os_.get(want["b_also"].lower(), 0), "census": census(o["text"])}
        if refused(o):
            entry["result"] = "RED: the runner refuses --mods-dir"
        elif o["rc"] != 0:
            entry["result"] = "RED: exited %d" % o["rc"]
        elif entry["wins_draws"] >= order_fx["min_draws"] and not any(entry["other_draws"].values()) and \
                entry["b_also_draws"] >= order_fx["min_draws"]:
            entry["result"] = "GREEN"
        else:
            entry["result"] = "RED: %s" % {k: entry[k] for k in ("wins_draws", "other_draws", "b_also_draws")}
        check[order] = entry
        if entry["result"] != "GREEN":
            red.append("order %s: %s" % (order, entry["result"]))
    report["checks"]["order"] = check

    summary = "text '%s' x%d; chunk %s; order %s; none %s" % (
        changed, report["checks"]["text"].get("mod_changed_draws", 0), report["checks"]["chunk"].get("census"),
        "/".join(e["result"].split(":")[0] for e in check.values()),
        "/".join(report["checks"]["none_" + l["name"]]["result"].split(":")[0] for l in det_fx["legs"]))
    if red:
        return finish(1, "RED -- " + "; ".join(red))
    return finish(0, "GREEN -- " + summary)


if __name__ == "__main__":
    sys.exit(main())
