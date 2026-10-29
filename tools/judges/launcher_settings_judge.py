#!/usr/bin/env python3
"""tools/judges/launcher_settings_judge.py -- the launcher's Settings page against the game folder's ps2r.ini.

Through the engine's Qt Test driver test_launcher_settings (the page's own controls, in a fresh home folder whose game
folder the setup's layout made), every row is judged against the FROZEN fixture fixtures/judges/launcher_settings_pal.json:

  1. the row exists under the fixture's key, with the fixture's label and kind;
  2. every fixture value, set through the row's control (the launcher's model writes the key), is in the file under the
     key, and reads back through the model; a value written to the file by someone else (the game's Options menu) is what
     the row shows after a reload;
  3. a folder row writes its path relative to the game folder when inside it and absolute otherwise, and reads back
     absolute;
  4. the key is the game's own: written by the game's Options menu (the key in a SettingsFile::Set / Persist call of the
     fixture's menu sources), or documented in the README's key list;
  5. the two language rows offer what the game offers for the disc (--iso: the game folder's disc) and the language packs
     in its mods folder (the repo's mods/ copied in, as the install places the release's): the fixture's values exactly, in
     order; the legacy game.language (its retired "arabic+french" form) reads as text arabic / speech french, and a change
     through a row writes both keys and drops game.language.

RED when there is no launcher, a fixture row is missing, or anything above fails; GREEN otherwise.

  tools/judges/launcher_settings_judge.py --launcher-build DIR --out DIR [--fixture FILE] [--manifest launcher/title.json]
                                          [--iso IMAGE (the disc row)]

Exit 0 GREEN, 1 RED. Writes <out>/report.json and <out>/settings.log.
"""
import argparse
import json
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "launcher_settings_pal.json"))
    ap.add_argument("--manifest", default=os.path.join(ROOT, "launcher", "title.json"))
    ap.add_argument("--iso", default="", help="a disc image for the disc row (optional)")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_settings_judge", "result": "RED", "reasons": [], "fixture": os.path.relpath(a.fixture, ROOT), "rows": []}

    def finish():
        report["result"] = "GREEN" if not report["reasons"] else "RED"
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        ok = sum(1 for r in report["rows"] if r.get("ok"))
        print("launcher_settings_judge: %s -- %d of %d rows round-trip under the game's keys" % (report["result"], ok, len(fx["rows"])))
        for r in report["reasons"]:
            print("  - " + r)
        return 0 if report["result"] == "GREEN" else 1

    driver = os.path.join(a.launcher_build, "test_launcher_settings" + (".exe" if os.name == "nt" else ""))
    if not os.access(driver, os.X_OK):
        report["reasons"].append("no launcher: %s does not exist" % driver)
        return finish()
    home = tempfile.mkdtemp(prefix="te-launcher-settings-home-")
    titles = tempfile.mkdtemp(prefix="te-launcher-settings-titles-")
    shutil.copy(a.manifest, os.path.join(titles, "title.json"))
    discs = os.path.join(os.path.dirname(a.manifest), "discs")
    if os.path.isdir(discs):
        shutil.copytree(discs, os.path.join(titles, "discs"))
    driver_report = os.path.join(a.out, "settings_report.json")
    env = dict(os.environ, HOME=home, USERPROFILE=home, LOCALAPPDATA=os.path.join(home, "AppData", "Local"), QT_QPA_PLATFORM="offscreen",
               PS2R_SETTINGS_MANIFEST=os.path.join(titles, "title.json"), PS2R_SETTINGS_REPORT=driver_report)
    if a.iso:
        env["PS2R_SETTINGS_ISO"] = a.iso
        env["PS2R_SETTINGS_PACKS"] = os.path.join(ROOT, fx.get("language_packs", "mods"))
    with open(os.path.join(a.out, "settings.log"), "w") as log:
        try:
            rc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=600).returncode
        except subprocess.TimeoutExpired:
            rc = -9
    report["driver_exit"] = rc
    if rc != 0:
        report["reasons"].append("the driver failed (exit %d): see settings.log" % rc)
    checked = json.load(open(driver_report)) if os.path.isfile(driver_report) else {}
    report["settings_file"] = checked.get("settings_file", "")
    want_file = os.path.join(home, fx["settings_file"])
    if os.path.realpath(report["settings_file"] or "/nonexistent") != os.path.realpath(want_file):
        report["reasons"].append("the launcher writes %s, not the game folder's %s" % (report["settings_file"] or "nothing", want_file))
    by_key = {r.get("key"): r for r in checked.get("rows", [])}
    menu_text = ""
    for src in fx["menu_sources"]:
        with open(os.path.join(ROOT, src), encoding="utf-8", errors="replace") as f:
            menu_text += f.read()
    readme = open(os.path.join(ROOT, fx["readme"]), encoding="utf-8").read()
    game_folder = os.path.dirname(want_file)
    for want in fx["rows"]:
        key = want["key"]
        got = by_key.get(key)
        row = {"key": key, "label": want["label"], "kind": want["kind"], "ok": False}
        report["rows"].append(row)
        if not got:
            report["reasons"].append("no Settings row for %s (%s)" % (key, want["label"]))
            continue
        problems = []
        label = got.get("shown_label") or got.get("label")
        if label != want["label"]:
            problems.append("its label is %r, not %r" % (label, want["label"]))
        if got.get("kind") != want["kind"]:
            problems.append("it is a %s, not a %s" % (got.get("kind"), want["kind"]))
        if got.get("round_trip") is not True:
            problems.append("it did not round-trip")
        if want["kind"] == "folder":
            fv = got.get("file_values", [])
            rb = got.get("read_back", [])
            vals = got.get("values", [])
            if len(fv) < 2 or fv[0] != want["default"] + "-moved":
                problems.append("a folder inside the game folder was not written relative (%r)" % (fv[:1],))
            if len(fv) < 2 or not os.path.isabs(fv[1]):
                problems.append("a folder outside the game folder was not written absolute (%r)" % (fv[1:2],))
            if rb != vals:
                problems.append("the folders did not read back as written (%r vs %r)" % (rb, vals))
            if vals and os.path.dirname(vals[0]) != game_folder:
                problems.append("the folder row is not anchored at the game folder")
            row["file_values"] = fv
        else:
            missing = [v for v in want["values"] if v not in got.get("values", [])]
            if missing:
                problems.append("the values %s were not offered" % ", ".join(missing))
            if want.get("choices_from"):
                # a derived row: exactly the disc's languages (and the packs), in the game's order
                if got.get("choices_from") != want["choices_from"]:
                    problems.append("its choices are not derived (%s), want %s" % (got.get("choices_from"), want["choices_from"]))
                if a.iso and got.get("values") != want["values"]:
                    problems.append("it offers %s, the disc and the packs give %s" % (got.get("values"), want["values"]))
            if got.get("file_values") != got.get("values"):
                problems.append("the file did not hold what the row set (%r vs %r)" % (got.get("file_values"), got.get("values")))
            row["values"] = got.get("values")
        # the key is the game's own
        if want["menu"] == "written":
            if not re.search(r'"%s"' % re.escape(key), menu_text):
                problems.append("the game's Options menu does not write %s (%s)" % (key, ", ".join(fx["menu_sources"])))
            row["game_writes_key"] = True
        elif want["menu"] in ("readme", "folder"):
            documented = key in readme
            row["documented_in_readme"] = documented
            if want["menu"] == "readme" and not documented:
                problems.append("%s is not in the README's key list" % key)
        row["problems"] = problems
        row["ok"] = not problems
        for p in problems:
            report["reasons"].append("%s (%s): %s" % (want["label"], key, p))
    # 5. the legacy key read as the two rows, and dropped once a row is written
    if fx.get("legacy"):
        lg = (by_key.get("game.language") or {}).get("legacy") or {}
        report["legacy"] = lg
        want = fx["legacy"]
        if a.iso:
            if lg.get("read_text") != want["read_text"] or lg.get("read_speech") != want["read_speech"]:
                report["reasons"].append("game.language = %s reads as text %r / speech %r (want %r / %r)" % (want["value"], lg.get("read_text"),
                                         lg.get("read_speech"), want["read_text"], want["read_speech"]))
            if lg.get("written_text") != want["read_text"] or lg.get("written_speech") != want["written_speech"] or lg.get("legacy_line_left") is not False:
                report["reasons"].append("a change through the speech row wrote text %r / speech %r, the legacy line left %s (want %r / %r, dropped)" % (
                    lg.get("written_text"), lg.get("written_speech"), lg.get("legacy_line_left"), want["read_text"], want["written_speech"]))
    report["settings_text"] = checked.get("settings_text", "")
    shutil.rmtree(home, ignore_errors=True)
    shutil.rmtree(titles, ignore_errors=True)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
