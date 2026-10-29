#!/usr/bin/env python3
"""tools/judges/launcher_mods_judge.py -- the launcher's Mods page against a mods folder.

Through the engine's Qt Test driver test_launcher_mods (the page's own controls, offscreen, in a fresh home folder whose game
folder the setup's layout made, the disc the development image), judged against the FROZEN fixture
fixtures/judges/launcher_mods_pal.json:

  1. the rows: the built-ins first (level select without a switch, speedrun mode with one), then the folder's mods by id
     (no mods.order yet) with an unreadable one last -- the language pack in the folder (mods/arabic) on NO row, and its
     earlier version's switch and order place dropped at the next write; every row's name, byline, region badge, summary and warning equal the
     fixture's frozen lines AND the lines the judge computes from that mod's mod.json;
  2. a switch writes mods.<id>.enabled = on / off to the game folder's ps2r.ini;
  3. Move down, a drag's drop (the slot) and the page's own drop event (a row released below the last one) write the whole
     order to mods.order, and the page lists in it;
  4. Remove asks first: Cancel keeps the folder; Remove deletes it and drops its mods.<id>.enabled line and its place in
     mods.order;
  5. a dropped mod folder is copied in; the same id again is ONE RULE: the page asks "Replace <name>
     <version>?" -- "The copy you have is removed first." -- declined nothing changes, accepted the new copy takes the old
     one's place with its switch kept; a mod made over the North American disc (the fixture's usa_mod: its archive recipe
     records the North American archive, as an import writes it) shows its NTSC-U badge, "made for the North American disc"
     in its summary on this European game folder, its notes and the Crate Mod Loader settings it asks for and "Left
     out on this disc: made for the North American disc." first in its warning lines (the game's base check leaves it out
     here, and the game's own Mods page no longer says so);
  6. a dropped Mod Crate folder (made at run time from the disc, never kept) is handed to the import library: with the stub
     the page shows the stub's sentence, with the library a new row appears with its mod.json;
  6c. the same crate dropped again asks first: declined nothing is imported, accepted the import replaces the mod (one row,
     its mod.json rewritten);
  7. the same drop cancelled at once is stopped with nothing added and nothing left behind; without a disc the import is
     refused with its sentence; no visible text carries developer vocabulary.

RED when the driver does not exist (a launcher without this page), or any check fails; GREEN otherwise.

  tools/judges/launcher_mods_judge.py --launcher-build DIR --iso IMAGE --out DIR [--fixture FILE] [--manifest launcher/title.json]

Exit 0 GREEN, 1 RED, 2 the setup failed. Writes <out>/report.json and <out>/mods.log.
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
sys.path.insert(0, os.path.join(ROOT, "tools", "mods"))


# --- the page's lines, from a mod.json (the launcher's mod_list.cpp is the C++ of the same rules)
REGION_DISC = {"PAL": "the European disc", "NTSC-U": "the North American disc", "NTSC-J": "the Japanese disc"}


def disc_region(iso_reader):
    """The disc's region from the program SYSTEM.CNF boots (its serial's prefix), as the launcher's identity says it."""
    e = iso_reader.find("SYSTEM.CNF")
    text = iso_reader.read(e["lba"], 0, e["size"]).decode("latin-1") if e else ""
    m = re.search(r"BOOT2\s*=\s*cdrom0:\\?([A-Z]{4})", text, re.I)
    prefix = m.group(1).upper() if m else ""
    return {"SLES": "PAL", "SCES": "PAL", "SCED": "PAL", "SLED": "PAL", "SLUS": "NTSC-U", "SCUS": "NTSC-U",
            "SLPM": "NTSC-J", "SLPS": "NTSC-J", "SCPS": "NTSC-J"}.get(prefix, "")


def base_checked(manifest):
    """A recipe of files[] records the file it was made over (form "ranges" with base_bytes: what the importer writes): the
    game composes the mod only over that very file (its base check, platform/nativeee/host/mods_disc.cpp)."""
    for f in manifest.get("files", []) or []:
        b = f.get("base_bytes")
        if f.get("form") == "ranges" and isinstance(b, (int, float)) and not isinstance(b, bool) and b > 0:
            return True
    return False


def expected_lines(manifest, disc_region_name=""):
    raw_region = ((manifest.get("base") or {}).get("region", "") or "").strip()
    region = "" if not raw_region else ("Any region" if raw_region.lower() == "any" else raw_region.upper())
    made, here = REGION_DISC.get(raw_region.upper(), ""), REGION_DISC.get(disc_region_name, "")
    # made for another region's disc than the one the game starts with: the summary names its disc; left out there
    # too when a recipe records its base (the game's base check): the warning's first line
    other = bool(made and here and made != here)
    left_out = bool(other and base_checked(manifest))
    parts = []
    author, version = (manifest.get("author") or "").strip(), manifest.get("version")
    version = ("%g" % version) if isinstance(version, (int, float)) else (version or "").strip()
    if author:
        parts.append("by %s" % author)
    if version:
        parts.append(version)
    byline = " · ".join(parts)
    if True:   # (a language pack has no row)
        files = [f.get("path") for f in manifest.get("files", []) if f.get("path")]
        bits = []
        if len(files) == 1:
            bits.append("Changes %s" % files[0])
        elif len(files) > 1:
            bits.append("Changes %d files" % len(files))
        data = sum(max(0, v) for v in ((manifest.get("executable") or {}).get("data_changes") or {}).values() if isinstance(v, (int, float)))
        if data > 0:
            bits.append("data changes in the executable" if bits else "Data changes in the executable")
        if bits:
            summary = "; ".join(bits) + (", made for %s." % made if other else ".")
        else:
            d = " ".join((manifest.get("description") or "").split())
            m = re.search(r"[.!?](\s|$)", d)
            summary = d[:m.start() + 1] if m else d
            if other:
                summary = ("%s Made for %s." % (summary, made)) if summary else "Made for %s." % made
    code = [c for c in ((manifest.get("executable") or {}).get("code_changes") or []) if c]
    lines = []
    if left_out:
        lines.append("Left out on this disc: made for %s." % made)
    if code:
        w = "Code changes not supported: %s" % ", ".join(code[:3])
        if len(code) > 3:
            w += " and %d more" % (len(code) - 3)
        lines.append(w + ".")
    settings = [g.strip() for g in ((manifest.get("import") or {}).get("game_code_settings") or []) if isinstance(g, str) and g.strip()]
    if settings:
        lines.append("It asks for Crate Mod Loader settings the game cannot apply: %s." % ", ".join(settings))
    notes = [n.strip() for n in (manifest.get("notes") or []) if isinstance(n, str) and n.strip()]
    return {"name": manifest.get("name") or manifest.get("id"), "byline": byline, "region": region, "summary": summary,
            "warning": "\n".join(lines), "notes": "\n".join(notes)}


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True)
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "launcher_mods_pal.json"))
    ap.add_argument("--manifest", default=os.path.join(ROOT, "launcher", "title.json"))
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_mods_judge", "result": "RED", "reasons": [], "fixture": os.path.relpath(a.fixture, ROOT), "checks": {}}

    def finish(code=None):
        report["result"] = "GREEN" if not report["reasons"] else "RED"
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_mods_judge: %s -- %d checks" % (report["result"], len(report["checks"])))
        for r in report["reasons"]:
            print("  - " + r)
        if code is not None:
            return code
        return 0 if report["result"] == "GREEN" else 1

    driver = os.path.join(a.launcher_build, "test_launcher_mods" + (".exe" if os.name == "nt" else ""))
    if not os.access(driver, os.X_OK):
        report["reasons"].append("no Mods page driver: %s does not exist (a launcher without this page)" % driver)
        return finish()
    work = tempfile.mkdtemp(prefix="te-launcher-mods-")
    try:
        # --- the mods folder: the order pair from the disc, the code-changes mod, the language mod, an unreadable one
        mods = os.path.join(work, "fixture-mods")
        gen = subprocess.run([sys.executable, os.path.join(ROOT, fx["generator"]), "--iso", a.iso, "--out", mods, "--kinds", fx["generator_kinds"]],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, cwd=ROOT)
        if gen.returncode != 0:
            report["reasons"].append("the fixture generator failed: %s" % gen.stdout.strip().splitlines()[-1:])
            return finish(2)
        os.remove(os.path.join(mods, "report.json"))
        code = fx["code_mod"]
        os.makedirs(os.path.join(mods, code["id"], "disc"))
        with open(os.path.join(mods, code["id"], "mod.json"), "w", newline="\n") as f:
            json.dump(code["manifest"], f, indent=2)
        shutil.copytree(os.path.join(ROOT, fx["language_mod"]), os.path.join(mods, os.path.basename(fx["language_mod"])))
        os.makedirs(os.path.join(mods, fx["broken_mod"]["id"]))
        with open(os.path.join(mods, fx["broken_mod"]["id"], "mod.json"), "w") as f:
            f.write(fx["broken_mod"]["text"])
        manifests = {}
        for d in sorted(os.listdir(mods)):
            try:
                manifests[d] = json.load(open(os.path.join(mods, d, "mod.json"), encoding="utf-8"))
            except ValueError:
                manifests[d] = None
        # --- the Mod Crate folder: ModCrateInfo.txt and one layer0 file, the disc's with one line changed
        from make_fixture_overlay import Iso   # the generator's ISO 9660 reader (test-only Python)
        iso = Iso(a.iso)
        entry = iso.find(fx["crate"]["layer0_file"])
        if not entry:
            report["reasons"].append("the disc has no %s" % fx["crate"]["layer0_file"])
            return finish(2)
        data = iso.read(entry["lba"], 0, entry["size"])
        old, new = (s.encode("latin-1") for s in fx["crate"]["change"])
        if old not in data:
            report["reasons"].append("%s on this disc has no '%s'" % (fx["crate"]["layer0_file"], fx["crate"]["change"][0]))
            return finish(2)
        crate = os.path.join(work, "Downloads", fx["crate"]["folder"])
        os.makedirs(os.path.join(crate, "layer0"))
        with open(os.path.join(crate, "ModCrateInfo.txt"), "w", newline="\n") as f:
            f.write(fx["crate"]["info"])
        with open(os.path.join(crate, "layer0", fx["crate"]["layer0_file"]), "wb") as f:
            f.write(data.replace(old, new, 1))
        region_here = disc_region(iso)
        report["disc_region"] = region_here
        # --- a mod folder made over the North American disc (no payload of the game: an empty file)
        usa_fx = fx["usa_mod"]
        usa = os.path.join(work, "Downloads", usa_fx["folder"])
        os.makedirs(os.path.join(usa, "disc", "CRASH6"))
        with open(os.path.join(usa, "mod.json"), "w", newline="\n", encoding="utf-8") as f:
            json.dump(usa_fx["manifest"], f, indent=2, ensure_ascii=False)
        for row in usa_fx["manifest"].get("files", []):
            path = os.path.join(usa, "disc", *row["path"].split("/"))
            if row.get("form") == "ranges":   # a recipe (a few zeros: no game data)
                with open(path + ".ranges", "w", newline="\n") as f:
                    f.write("# the launcher mods judge's recipe (no game data)\nzero 2048\n")
            else:
                open(path, "wb").close()
        # --- the driver, in a fresh home
        home = os.path.join(work, "home")
        titles = os.path.join(work, "titles")
        os.makedirs(home)
        os.makedirs(titles)
        shutil.copy(a.manifest, os.path.join(titles, "title.json"))
        discs = os.path.join(os.path.dirname(a.manifest), "discs")
        if os.path.isdir(discs):
            shutil.copytree(discs, os.path.join(titles, "discs"))
        steps = fx["steps"]
        driver_report = os.path.join(a.out, "mods_report.json")
        env = dict(os.environ, HOME=home, USERPROFILE=home, LOCALAPPDATA=os.path.join(home, "AppData", "Local"), QT_QPA_PLATFORM="offscreen",
                   PS2R_MODS_MANIFEST=os.path.join(titles, "title.json"), PS2R_MODS_FIXTURE=mods, PS2R_MODS_ISO=os.path.abspath(a.iso),
                   PS2R_MODS_CRATE=crate, PS2R_MODS_USA=usa, PS2R_MODS_REPORT=driver_report, PS2R_MODS_SWITCH=steps["switch"],
                   PS2R_MODS_MOVE_DOWN=steps["move_down"], PS2R_MODS_DRAG=steps["drag"], PS2R_MODS_REMOVE=steps["remove"])
        with open(os.path.join(a.out, "mods.log"), "w") as log:
            try:
                rc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT, timeout=900).returncode
            except subprocess.TimeoutExpired:
                rc = -9
        report["driver_exit"] = rc
        if rc != 0:
            report["reasons"].append("the driver failed (exit %d): see mods.log" % rc)
        got = json.load(open(driver_report)) if os.path.isfile(driver_report) else {}
        report["import_kind"] = got.get("import_kind")

        # 1. the rows
        rows = got.get("rows", [])
        by_id = {r["id"]: r for r in rows}
        builtin_ids = [r["id"] for r in rows if r.get("group") == "builtin"]
        folder_ids = [r["id"] for r in rows if r.get("group") == "folder"]
        c = {"builtins": builtin_ids, "folder": folder_ids, "rows": []}
        want_builtins = [b["id"] for b in fx["builtins"]]
        if builtin_ids != want_builtins:
            report["reasons"].append("the built-ins listed are %s, not %s" % (builtin_ids, want_builtins))
        for b in fx["builtins"]:
            r = by_id.get(b["id"], {})
            if r.get("name") != b["name"] or bool(r.get("switchable")) != b["switchable"]:
                report["reasons"].append("built-in %s: %r, switch %s (want %r, switch %s)" % (b["id"], r.get("name"), r.get("switchable"), b["name"], b["switchable"]))
            if b.get("summary_starts") and not (r.get("summary") or "").startswith(b["summary_starts"]):
                report["reasons"].append("built-in %s: its line %r does not start %r" % (b["id"], r.get("summary"), b["summary_starts"]))
        want_folder = [w["id"] for w in fx["rows"]]
        # a language pack is not a mod: in the folder, on no row
        listed = [i for i in fx.get("not_listed", []) if i in by_id]
        c["not_listed"] = {"ids": fx.get("not_listed", []), "listed": listed}
        if listed:
            report["reasons"].append("a language pack is not a mod, yet the page lists %s" % listed)
        if fx.get("not_listed") and got.get("stale_pack_keys_dropped") not in fx["not_listed"]:
            report["reasons"].append("the language pack's stale mods.<id>.enabled / mods.order place were not dropped at a write (%r)" % got.get("stale_pack_keys_dropped"))
        if folder_ids != want_folder:
            report["reasons"].append("the folder's mods are listed %s, not %s (by id, the unreadable last)" % (folder_ids, want_folder))
        for w in fx["rows"]:
            r = by_id.get(w["id"])
            entry = {"id": w["id"], "shown": r}
            c["rows"].append(entry)
            if not r:
                report["reasons"].append("no row for %s" % w["id"])
                continue
            if w.get("broken"):
                if not r.get("broken") or r.get("summary") != w["summary"] or r.get("switchable"):
                    report["reasons"].append("%s: shown %r (broken %s, switch %s), want the unreadable row %r" % (w["id"], r.get("summary"), r.get("broken"), r.get("switchable"), w["summary"]))
                continue
            computed = expected_lines(manifests.get(w["id"]) or {}, region_here)
            entry["computed"] = computed
            for k in ("name", "byline", "region", "summary", "warning"):
                if r.get(k) != w[k]:
                    report["reasons"].append("%s: its %s is %r, the fixture's %r" % (w["id"], k, r.get(k), w[k]))
                if computed[k] != w[k]:
                    report["reasons"].append("%s: the %s computed from mod.json is %r, the fixture's %r" % (w["id"], k, computed[k], w[k]))
            if not r.get("switchable") or not r.get("open_folder") or not r.get("remove"):
                report["reasons"].append("%s: switch %s, Open folder %s, Remove %s" % (w["id"], r.get("switchable"), r.get("open_folder"), r.get("remove")))
        report["checks"]["rows"] = c

        # 2. the switch
        s = got.get("switch", {})
        report["checks"]["switch"] = s
        if s.get("id") != steps["switch"] or s.get("after_on") != "on" or s.get("after_off") != "off" or s.get("speedrun") != "on":
            report["reasons"].append("the switch of %s wrote %r then %r (speedrun %r)" % (s.get("id"), s.get("after_on"), s.get("after_off"), s.get("speedrun")))
        # 3. the order
        o = got.get("order", {})
        report["checks"]["order"] = o
        if o.get("after_move_down") != steps["after_move_down"] or ",".join(o.get("listed_after_move_down", [])) != steps["after_move_down"]:
            report["reasons"].append("Move down on %s: mods.order %r, listed %s (want %s)" % (steps["move_down"], o.get("after_move_down"), o.get("listed_after_move_down"), steps["after_move_down"]))
        if o.get("after_drag") != steps["after_drag"] or ",".join(o.get("listed_after_drag", [])) != steps["after_drag"]:
            report["reasons"].append("the drop of %s: mods.order %r, listed %s (want %s)" % (steps["drag"], o.get("after_drag"), o.get("listed_after_drag"), steps["after_drag"]))
        if o.get("drop_event") != steps["drop_event"] or o.get("after_drop_event") != steps["after_drop_event"]:
            report["reasons"].append("the page's drop event %s: mods.order %r (want %s)" % (o.get("drop_event"), o.get("after_drop_event"), steps["after_drop_event"]))
        # 4. Remove
        r = got.get("remove", {})
        report["checks"]["remove"] = r
        if r.get("id") != steps["remove"] or not r.get("cancel_kept") or not r.get("folder_gone") or not r.get("key_gone") or r.get("order_after") != steps["order_after_remove"]:
            report["reasons"].append("Remove %s: cancel kept %s, folder gone %s, key gone %s, mods.order %r (want %r)" % (
                r.get("id"), r.get("cancel_kept"), r.get("folder_gone"), r.get("key_gone"), r.get("order_after"), steps["order_after_remove"]))
        # 5. a mod folder copied
        cp = got.get("copy", {})
        report["checks"]["copy"] = cp
        if not cp.get("row") or not cp.get("copied_file") or cp.get("error"):
            report["reasons"].append("a dropped mod folder was not copied in: %r" % cp.get("notice"))
        rp = fx["replace"]
        dec, acc = cp.get("declined") or {}, cp.get("accepted") or {}
        if dec.get("title") != rp["folder_title"] or dec.get("text") != rp["text"]:
            report["reasons"].append("the same mod folder again: the sheet says %r / %r (want %r / %r)" % (dec.get("title"), dec.get("text"), rp["folder_title"], rp["text"]))
        if dec.get("notice") != rp["folder_declined"] or not dec.get("kept_old_copy") or not dec.get("sheet_closed"):
            report["reasons"].append("declined: %r, old copy kept %s, sheet closed %s" % (dec.get("notice"), dec.get("kept_old_copy"), dec.get("sheet_closed")))
        if acc.get("notice") != rp["folder_accepted"] or acc.get("error") or not acc.get("new_copy") or not acc.get("switch_kept") or acc.get("left_in_import_tmp"):
            report["reasons"].append("accepted: %r (new copy %s, switch kept %s, left behind %s)" % (acc.get("notice"), acc.get("new_copy"), acc.get("switch_kept"), acc.get("left_in_import_tmp")))
        # 5b. the North American mod's row on this game folder
        ur = cp.get("usa_row") or {}
        computed = expected_lines(usa_fx["manifest"], region_here)
        report["checks"]["usa_row"] = {"shown": ur, "computed": computed, "disc_region": region_here, "launcher_disc_region": cp.get("disc_region")}
        for k in ("name", "byline", "region", "summary", "warning", "notes"):
            if ur.get(k) != usa_fx["row"][k]:
                report["reasons"].append("the North American mod: its %s is %r, the fixture's %r" % (k, ur.get(k), usa_fx["row"][k]))
            if computed[k] != usa_fx["row"][k]:
                report["reasons"].append("the North American mod: the %s computed from mod.json is %r, the fixture's %r" % (k, computed[k], usa_fx["row"][k]))
        if cp.get("disc_region") != region_here:
            report["reasons"].append("the launcher takes the disc for %r, the disc's program says %r" % (cp.get("disc_region"), region_here))
        # 6. the import
        i = got.get("import", {})
        report["checks"]["import"] = i
        if got.get("import_kind") == "library":
            if len(i.get("new_rows", [])) != 1 or not i.get("new_mod_json") or i.get("error"):
                report["reasons"].append("with the import library the crate did not become a mod: %r, new rows %s" % (i.get("notice"), i.get("new_rows")))
        else:
            if i.get("notice") != fx["stub_refusal"] or not i.get("error") or i.get("new_rows"):
                report["reasons"].append("with the stub the crate's notice is %r (want %r), new rows %s" % (i.get("notice"), fx["stub_refusal"], i.get("new_rows")))
        # 6c. the same crate again: asked first; declined nothing, accepted the import replaces the mod
        ri = got.get("replace_import", {})
        report["checks"]["replace_import"] = ri
        if got.get("import_kind") == "library":
            if ri.get("title") != rp["crate_title"] or ri.get("text") != rp["text"]:
                report["reasons"].append("the same crate again: the sheet says %r / %r (want %r / %r)" % (ri.get("title"), ri.get("text"), rp["crate_title"], rp["text"]))
            if ri.get("declined_notice") != rp["crate_declined"] or ri.get("declined_importing"):
                report["reasons"].append("the crate declined: %r (importing %s)" % (ri.get("declined_notice"), ri.get("declined_importing")))
            if ri.get("accepted_notice") != rp["crate_accepted"] or ri.get("accepted_error") or ri.get("rows_after") != ri.get("rows_before") or not ri.get("mod_json_rewritten"):
                report["reasons"].append("the crate accepted: %r (rows %s -> %s, mod.json rewritten %s)" % (ri.get("accepted_notice"), ri.get("rows_before"), ri.get("rows_after"), ri.get("mod_json_rewritten")))
        # 6b. Cancel: the same drop cancelled at once -- stopped, nothing added, nothing left in the import's temporary folder
        cn = got.get("cancel", {})
        report["checks"]["cancel"] = cn
        if cn.get("notice") != fx["cancel_notice"] or cn.get("error") or cn.get("left_in_import_tmp"):
            report["reasons"].append("Cancel: %r (error %s), left behind %s (want %r)" % (cn.get("notice"), cn.get("error"), cn.get("left_in_import_tmp"), fx["cancel_notice"]))
        # 7. no disc; clean text
        n = got.get("no_disc", {})
        report["checks"]["no_disc"] = n
        if n.get("notice") != fx["no_disc"] or n.get("importing"):
            report["reasons"].append("without a disc the import says %r (importing %s)" % (n.get("notice"), n.get("importing")))
        if got.get("visible_text_clean") is not True:
            report["reasons"].append("the visible texts were not all checked clean (the driver stopped early)")
        report["settings_text"] = got.get("settings_text", "")
        return finish()
    finally:
        shutil.rmtree(work, ignore_errors=True)


if __name__ == "__main__":
    sys.exit(main())
