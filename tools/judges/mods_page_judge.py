#!/usr/bin/env python3
"""mods_page_judge.py -- the game's Options > MODS page with a mods folder: does it list ONLY the mods that apply while the
game runs (the built-ins: level select, speedrun mode), with nothing for the folder's mods -- no row, no switch, no note --
while those mods still apply, and does a built-in's switch still apply at once? (a mod of the mods
folder is switched in the launcher only.)

The mods are made at run time in a temporary folder (never committed; deleted afterwards): the disc overlay judge's order
pair (tools/mods/make_fixture_overlay.py --kinds order: order_a / order_b change the OPTIONS page's line "graphic options"),
the fixture's code-changes mod (only a mod.json and an empty disc/ tree), a mod made over the North American disc (its
archive recipe records that disc's archive, as an import writes it: the base check leaves it out on this disc; the recipe is
the identity over this disc's archive) and a copy of the repo's language pack (mods/arabic: not a mod). A settings file
switches the order pair and the North American mod on -- the launcher's lines -- and keeps an earlier version's switch and
order place for the pack (fixtures/judges/mods_page_pal.json "settings"). Every run is deterministic and headless with
TE_GFX_DIAG_TEXT=1 (every text the game draws is logged with its string):

  (i)  PAGE     the route to OPTIONS > MODS (fixtures/padlogs/mods_page_toggle.padlog) with the settings file and the mods
                folder: the page draws its rows ('level select', 'speedrun mode : off') and no other '<name> : off|on' row; no
                folder mod's name, no note ('code changes not supported', 'made for the ...', 'at the next start', ...) and
                nothing of the pack is drawn in the whole run ("never_drawn"); the second row is speedrun mode and the route's
                CROSS switches it on at once (the log, 'speedrun mode : on', mods.speedrun.enabled = on in the settings file);
                the folder mods' lines of the settings file are as the launcher wrote them and none is added; the start-up log
                keeps the left-out line of the North American mod.
       APPLIED  in the same run the overlays apply: the OPTIONS page the route passes draws order_b's line (the later in
                mods.order) and its second change, never the disc's line, and the census row counts two layers.
  (ii) BASE     the OPTIONS route (fixtures/padlogs/options_graphic.padlog) with no settings file and no mods folder draws the
                disc's line (the route reaches the page: the reference for APPLIED).

A runner that refuses the run (exit 2: no --mods-dir) is RED.

Usage: mods_page_judge.py --bin BIN --iso ISO [--out DIR] [--fixture F] [--keep]
Writes OUT/report.json. Exit 0 = GREEN, 1 = RED, 2 = the setup failed (the disc, the generator, the vanilla run).
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
DEFAULT_FIXTURE = os.path.join(ROOT, "fixtures", "judges", "mods_page_pal.json")
STRING = re.compile(r"\[te\.gfx\.diag\] present (\d+) text \d+ quads .* string '(.*)'$")
CENSUS = re.compile(r"^\[twinsanityevolved\] mods=(\d+):(\d+):(\d+)\s*$", re.M)
sys.path.insert(0, os.path.join(ROOT, "tools", "mods"))


def run(binary, iso, out, tag, padlog, stop_at, extra, run_seconds):
    d = os.path.join(out, tag)
    shutil.rmtree(d, ignore_errors=True)
    mc = os.path.join(d, "mc")
    os.makedirs(mc)
    log = os.path.join(d, "run.log")
    args = [binary, "--iso", iso, "--deterministic", "--no-sdl-input", "--no-audio", "--skip-movies", "--trace-state",
            "--play-pad", os.path.join(ROOT, padlog), "--stop-at", str(stop_at), "--mc-dir", mc, "--run-seconds", str(run_seconds)] + extra
    env = dict(os.environ, TE_GFX_HEADLESS="1", TE_GFX_DIAG_TEXT="1")
    print("mods_page_judge: run %s: %s" % (tag, " ".join(args[1:])), flush=True)
    with open(log, "w") as f:
        rc = subprocess.call(args, stdout=subprocess.DEVNULL, stderr=f, env=env, cwd=ROOT)
    with open(log, errors="replace") as f:
        lines = f.read().splitlines()
    print("mods_page_judge: run %s exit %d -> %s" % (tag, rc, log), flush=True)
    return {"rc": rc, "log": log, "lines": lines, "text": "\n".join(lines)}


def frames(lines):
    """[(present, [strings in drawing order])] in log order"""
    out, cur, at = [], None, None
    for line in lines:
        m = STRING.match(line)
        if not m:
            continue
        p = int(m.group(1))
        if p != at:
            cur = [p, []]
            out.append(cur)
            at = p
        cur[1].append(m.group(2))
    return out


def counts(lines):
    c = {}
    for line in lines:
        m = STRING.match(line)
        if m:
            s = m.group(2).lower()
            c[s] = c.get(s, 0) + 1
    return c


def refused(r):
    return r["rc"] == 2 and ("usage:" in r["text"] or "--mods-dir" in r["text"] or "--mod " in r["text"])


def ini_lines(text):
    return [l.strip() for l in text.splitlines() if l.strip() and not l.strip().startswith("#")]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--bin", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "mods_page_judge"))
    ap.add_argument("--fixture", default=DEFAULT_FIXTURE)
    ap.add_argument("--keep", action="store_true", help="keep the generated mods and the runs")
    a = ap.parse_args()
    with open(a.fixture) as f:
        fx = json.load(f)
    binary, iso = os.path.abspath(a.bin), os.path.abspath(a.iso)
    shutil.rmtree(a.out, ignore_errors=True)
    os.makedirs(a.out)
    report = {"bin": binary, "iso": iso, "fixture": os.path.relpath(a.fixture, ROOT), "checks": {}, "verdict": None}

    def finish(code, line):
        report["verdict"] = line
        with open(os.path.join(a.out, "report.json"), "w") as f:
            json.dump(report, f, indent=2)
        print("mods_page_judge: %s" % line)
        if not a.keep:
            for name in os.listdir(a.out):
                p = os.path.join(a.out, name)
                if os.path.isdir(p):
                    shutil.rmtree(p, ignore_errors=True)
        return code

    # --- the mods folder: the order pair from the disc, the code-changes mod, the North American mod, the language pack
    mods = os.path.join(a.out, "mods")
    gen = subprocess.run([sys.executable, os.path.join(ROOT, fx["generator"]), "--iso", iso, "--out", mods, "--kinds", fx["generator_kinds"]],
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
    print(gen.stdout, end="")
    if gen.returncode != 0:
        return finish(2, "SETUP FAILED -- the fixture generator: %s" % gen.stdout.strip().splitlines()[-1:])
    os.remove(os.path.join(mods, "report.json"))   # not a mod folder
    code = fx["code_mod"]
    os.makedirs(os.path.join(mods, code["id"], "disc"))
    with open(os.path.join(mods, code["id"], "mod.json"), "w", newline="\n") as f:
        json.dump(code["manifest"], f, indent=2)
        f.write("\n")
    usa = fx["usa_mod"]
    from make_fixture_overlay import Iso   # the generator's ISO 9660 reader (test-only Python)
    archive = Iso(iso).find("CRASH6/CRASH.BD")
    if not archive:
        return finish(2, "SETUP FAILED -- the disc has no CRASH6/CRASH.BD")
    os.makedirs(os.path.join(mods, usa["id"], "disc", "CRASH6"))
    with open(os.path.join(mods, usa["id"], "mod.json"), "w", newline="\n") as f:
        json.dump(usa["manifest"], f, indent=2)
        f.write("\n")
    with open(os.path.join(mods, usa["id"], "disc", "CRASH6", "CRASH.BD.ranges"), "w", newline="\n") as f:
        f.write("# the mods page judge: the identity over this disc's archive (the base check refuses it first)\nbase 0 %d\n" % archive["size"])
    shutil.copytree(os.path.join(ROOT, fx["language_mod"]), os.path.join(mods, os.path.basename(fx["language_mod"])))
    ini = os.path.join(a.out, "ps2r.ini")
    with open(ini, "w", newline="\n") as f:
        f.write("# the mods page judge's settings file\n" + "".join(s + "\n" for s in fx["settings"]))
    with open(ini) as f:
        ini_before = f.read()
    report["mods"] = sorted(os.listdir(mods))
    page, applied, base = fx["page"], fx["applied"], fx["base_route"]
    red = []

    # --- (ii) BASE first: the route reaches the OPTIONS page and draws the disc's line
    v = run(binary, iso, a.out, "base_options", base["padlog"], base["stop_at"], ["--config", "none"], base["run_seconds"])
    vc = counts(v["lines"])
    report["checks"]["base"] = {"rc": v["rc"], "draws": vc.get(base["string"], 0)}
    if v["rc"] != 0 or vc.get(base["string"], 0) < base["min_draws"]:
        return finish(2, "SETUP FAILED -- the vanilla OPTIONS run (exit %d) drew '%s' %d times" % (v["rc"], base["string"], vc.get(base["string"], 0)))

    # --- (i) PAGE, and APPLIED in the same run
    p = run(binary, iso, a.out, "page", page["padlog"], page["stop_at"], ["--config", ini, "--mods-dir", mods], page["run_seconds"])
    check = {"rc": p["rc"]}
    with open(ini) as f:
        ini_after = f.read()
    check["ini_before"], check["ini_after"] = ini_before, ini_after
    key = "mods.%s.enabled" % page["toggled"]
    check["ini_key"] = next((l for l in ini_lines(ini_after) if l.startswith(key)), None)
    acheck = {}
    if refused(p):
        check["result"] = "RED: the runner refuses --mods-dir (no mods folder)"
        acheck["result"] = check["result"]
    elif p["rc"] != 0:
        check["result"] = "RED: the run exited %d" % p["rc"]
        acheck["result"] = check["result"]
    else:
        toggle_at = next((i for i, l in enumerate(p["lines"]) if page["toggle_log"] in l), None)
        before = p["lines"] if toggle_at is None else p["lines"][:toggle_at]
        after = [] if toggle_at is None else p["lines"][toggle_at:]
        call, cb, ca = counts(p["lines"]), counts(before), counts(after)
        page_frames = [fr for fr in frames(p["lines"]) if page["rows"][0] in fr[1]]
        row_re = re.compile(page["row_pattern"])
        page_rows = sorted({s for _, strings in page_frames for s in strings if row_re.search(s)})
        allowed = set(page["rows"]) | {page["row_after"]}
        check.update({
            "toggle_logged": toggle_at is not None,
            "page_frames": len(page_frames),
            "rows_drawn": {r: cb.get(r, 0) for r in page["rows"]},
            "rows_on_the_page": page_rows,
            "never_drawn": {w: sum(n for s, n in call.items() if w in s) for w in page["never_drawn"]},
            "row_after_draws": ca.get(page["row_after"], 0),
            "left_out_logged": any(page["left_out_log"] in l for l in p["lines"]),
            "page_strings_first_frame": page_frames[0][1] if page_frames else [],
        })
        problems = []
        if not page_frames:
            problems.append("the route never showed the Mods page")
        missing = [r for r, n in check["rows_drawn"].items() if n < page["min_draws"]]
        if missing:
            problems.append("rows not drawn before the switch: %s" % missing)
        others = [r for r in page_rows if r not in allowed]
        if others:
            problems.append("the page has other rows than the built-ins: %s" % others)
        drawn = {w: n for w, n in check["never_drawn"].items() if n}
        if drawn:
            problems.append("drawn, and never to be: %s" % drawn)
        if not check["toggle_logged"]:
            problems.append("no '%s' line: the route's CROSS did not switch %s" % (page["toggle_log"], page["toggled"]))
        if check["row_after_draws"] < page["min_draws"]:
            problems.append("the row reads '%s' %d times after the switch" % (page["row_after"], check["row_after_draws"]))
        if check["ini_key"] != key + " = on":
            problems.append("the settings file holds %r, not '%s = on'" % (check["ini_key"], key))
        kept = [k for k in page["folder_keys"] if k not in ini_lines(ini_after)]
        if kept:
            problems.append("the launcher's lines of the folder mods are not as they were: %s missing" % kept)
        written = [k for k in page["never_written"] if any(l.startswith(k) for l in ini_lines(ini_after))]
        if written:
            problems.append("the game wrote a folder mod's key: %s" % written)
        if not check["left_out_logged"]:
            problems.append("no start-up line '%s...'" % page["left_out_log"])
        check["result"] = "GREEN" if not problems else "RED: " + "; ".join(problems)
        rows = CENSUS.findall(p["text"])
        acheck = {"changed_draws": call.get(applied["changed"], 0), "also_draws": call.get(applied["also"], 0),
                  "original_draws": call.get(applied["original"], 0), "census": [int(x) for x in rows[-1]] if rows else None}
        aproblems = []
        if acheck["changed_draws"] < applied["min_draws"] or acheck["also_draws"] < applied["min_draws"]:
            aproblems.append("'%s' drawn %d times, '%s' %d times" % (applied["changed"], acheck["changed_draws"], applied["also"], acheck["also_draws"]))
        if acheck["original_draws"]:
            aproblems.append("the disc's '%s' drawn %d times" % (applied["original"], acheck["original_draws"]))
        if not acheck["census"] or acheck["census"][0] != applied["census_layers"]:
            aproblems.append("census %s (want %d layers)" % (acheck["census"], applied["census_layers"]))
        acheck["result"] = "GREEN" if not aproblems else "RED: " + "; ".join(aproblems)
    report["checks"]["page"] = check
    report["checks"]["applied"] = acheck
    if check["result"] != "GREEN":
        red.append("page: " + check["result"])
    if acheck["result"] != "GREEN":
        red.append("applied: " + acheck["result"])

    if red:
        return finish(1, "RED -- " + "; ".join(red))
    pc, ac = report["checks"]["page"], report["checks"]["applied"]
    return finish(0, "GREEN -- the page: only %s (%s), nothing of the folder's mods or the pack (%d strings x0), '%s' x%d after the "
                     "CROSS, %s, the launcher's lines kept, the left-out line logged; applied: '%s' x%d, '%s' x%d, '%s' x0, census %s"
                  % (" / ".join(page["rows"]), " ".join("x%d" % n for n in pc["rows_drawn"].values()), len(page["never_drawn"]),
                     page["row_after"], pc["row_after_draws"], pc["ini_key"], applied["changed"], ac["changed_draws"],
                     applied["also"], ac["also_draws"], applied["original"], ac["census"]))


if __name__ == "__main__":
    sys.exit(main())
