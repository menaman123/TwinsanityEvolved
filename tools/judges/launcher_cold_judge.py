#!/usr/bin/env python3
"""tools/judges/launcher_cold_judge.py -- the launcher's cold run of the prebuilt-install flow, judged.

With HOME pointed at an EMPTY folder holding only the player's disc image (hard-linked into Downloads -- the original file
is only read) and the launcher's own settings file naming a release server, the launcher (driven by the engine's Qt Test
program test_launcher_cold, which presses the window's own buttons) must: open the setup by itself; find the disc in
Downloads and identify it by content (PAL, original disc); keep the suggested game folder; keep This computer; download
the release zip, check it against SHA256SUMS and place it; Play; and the game must reach its title state (Title (7)) in
the launcher's own play log. Zero commands typed. The wall time and the bytes downloaded are recorded.

THE UPDATE-AVAILABLE CASE: the list also names a NEWER LAUNCHER -- a release published after the
driver was made, carrying this computer's launcher file (title.json release.launcher_assets) and its own SHA256SUMS. The
launcher's check at start must offer it on the Games page and on About; the driver is not a launcher installed from a
release (no BUILD-ID), so the offer must say it cannot replace itself and give the release's page -- and must not
download the file. The game's own Update is not offered for a launcher release. (The real replacement -- two packaged
launchers, the swap, the relaunch, Gatekeeper -- is tools/judges/launcher_update_judge.py.)

The release server is OFFLINE-SAFE: a Python http.server on 127.0.0.1 standing in for the GitHub releases API and the
asset download. It serves one release whose asset for this computer is a zip the judge makes from --game (a game build:
build/twinsanityevolved by default) in the shape tools/ci/release_dist.sh stages (the binary, README.md's user half, LICENSE,
BUILD-ID, THIRD_PARTY_NOTICES.md), with SHA256SUMS.

RED when the launcher has no prebuilt-install flow (the driver lacks the flow's test functions) or any step fails;
GREEN when every step holds. Frozen: fixtures/judges/launcher_cold_pal.json (the flow, the folder's contents, the
title state, the time limits).

  tools/judges/launcher_cold_judge.py --launcher-build DIR --iso IMAGE --out DIR [--game BIN]
                                      [--fixture fixtures/judges/launcher_cold_pal.json] [--manifest launcher/title.json]
                                      [--keep-home]

Writes <out>/report.json, <out>/cold.log, <out>/screenshots/*.png; exit 0 GREEN, 1 RED.
"""
import argparse
import hashlib
import http.server
import json
import os
import platform
import shutil
import socketserver
import subprocess
import sys
import tempfile
import threading
import time
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
BASE_GAME = os.path.join(ROOT, "build", "twinsanityevolved")


def host_target():
    m = platform.machine().lower()
    s = platform.system()
    if s == "Darwin":
        return "macos-arm64" if m == "arm64" else "macos-x86_64"
    if s == "Windows":
        return "windows-x64"
    return "linux-aarch64" if m in ("arm64", "aarch64") else "linux-x86_64"


def app_data(home):
    s = platform.system()
    if s == "Darwin":
        return os.path.join(home, "Library", "Application Support", "TwinsanityEvolved")
    if s == "Windows":
        return os.path.join(home, "AppData", "Local", "TwinsanityEvolved")
    return os.path.join(home, ".local", "share", "TwinsanityEvolved")


def release_zip(game, target, tag, out_dir):
    """The release asset as tools/ci/release_dist.sh stages it: twinsanityevolved-<target>/ with the game and its texts."""
    exe = "twinsanityevolved.exe" if target == "windows-x64" else "twinsanityevolved"
    name = "twinsanityevolved-%s-%s.zip" % (tag, target)
    path = os.path.join(out_dir, name)
    readme = []
    for line in open(os.path.join(ROOT, "README.md"), encoding="utf-8"):
        if line.startswith("## Build"):
            break
        readme.append(line)
    commit = subprocess.run(["git", "-C", ROOT, "rev-parse", "--short=12", "HEAD"], capture_output=True, text=True).stdout.strip()
    folder = "twinsanityevolved-" + target
    with zipfile.ZipFile(path, "w", zipfile.ZIP_DEFLATED) as z:
        info = zipfile.ZipInfo(folder + "/")
        info.external_attr = (0o40755 << 16) | 0x10
        z.writestr(info, b"")
        info = zipfile.ZipInfo(folder + "/" + exe)
        info.external_attr = (0o100755 << 16)
        info.compress_type = zipfile.ZIP_DEFLATED
        with open(game, "rb") as f:
            z.writestr(info, f.read())
        for fname, data in (("README.md", "".join(readme)), ("LICENSE", open(os.path.join(ROOT, "LICENSE")).read()),
                            ("BUILD-ID", "twinsanityevolved %s (%s) -- the cold judge's release\n" % (commit, target)),
                            ("THIRD_PARTY_NOTICES.md", "# The cold judge's release: the game build %s, zipped for the launcher.\n" % game)):
            info = zipfile.ZipInfo(folder + "/" + fname)
            info.external_attr = (0o100644 << 16)
            info.compress_type = zipfile.ZIP_DEFLATED
            z.writestr(info, data)
        # the repository's own mods, as release_dist.sh stages them (mods/<id>/ with a mod.json; the compiled-in C mods excepted)
        mods_root = os.path.join(ROOT, "mods")
        for mod_id in sorted(os.listdir(mods_root)) if os.path.isdir(mods_root) else []:
            mj = os.path.join(mods_root, mod_id, "mod.json")
            if not os.path.isfile(mj) or '"api": "c"' in open(mj, encoding="utf-8").read():
                continue
            for dirpath, _dirs, files in os.walk(os.path.join(mods_root, mod_id)):
                for fname in files:
                    full = os.path.join(dirpath, fname)
                    rel = os.path.relpath(full, mods_root)
                    info = zipfile.ZipInfo(folder + "/mods/" + rel.replace(os.sep, "/"))
                    info.external_attr = (0o100644 << 16)
                    info.compress_type = zipfile.ZIP_DEFLATED
                    with open(full, "rb") as f:
                        z.writestr(info, f.read())
    sha = hashlib.sha256(open(path, "rb").read()).hexdigest()
    return name, path, sha


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve(files):
    """The release server on a free port of 127.0.0.1; state["releases"] (bytes) is what /releases answers, read per request
    (it names the port, so it is filled in once the server listens)."""
    state = {"releases": b"[]", "bytes": 0, "requests": []}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            state["requests"].append(self.path)
            if self.path.startswith("/releases"):
                body, ctype = state["releases"], "application/json"
            elif self.path.startswith("/download/") and self.path[len("/download/"):] in files:
                with open(files[self.path[len("/download/"):]], "rb") as f:
                    body = f.read()
                ctype = "application/octet-stream"
            else:
                self.send_response(404)
                self.end_headers()
                return
            self.send_response(200)
            self.send_header("Content-Type", ctype)
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            self.wfile.write(body)
            state["bytes"] += len(body)

    httpd = Server(("127.0.0.1", 0), Handler)
    threading.Thread(target=httpd.serve_forever, daemon=True).start()
    return httpd, state


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--launcher-build", required=True, help="the launcher's build dir (holds test_launcher_cold)")
    ap.add_argument("--iso", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--game", default=BASE_GAME, help="the game build zipped as the release (default: build/twinsanityevolved)")
    ap.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "launcher_cold_pal.json"))
    ap.add_argument("--manifest", default=os.path.join(ROOT, "launcher", "title.json"))
    ap.add_argument("--keep-home", action="store_true")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_cold_judge", "result": "RED", "reasons": [], "fixture": os.path.relpath(a.fixture, ROOT)}

    def finish(code=None):
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_cold_judge: %s -- %s" % (report["result"], "; ".join(report["reasons"]) or "every step held"))
        for k in ("wall_seconds", "download_bytes", "served_bytes", "install_seconds", "title_seconds"):
            if k in report:
                print("  %s: %s" % (k, report[k]))
        return code if code is not None else (0 if report["result"] == "GREEN" else 1)

    driver = os.path.join(a.launcher_build, "test_launcher_cold" + (".exe" if os.name == "nt" else ""))
    if not os.access(driver, os.X_OK):
        report["reasons"].append("no launcher: %s does not exist" % driver)
        return finish()
    # the flow is the driver's: it must carry the prebuilt-install flow's steps
    funcs = subprocess.run([driver, "-functions"], capture_output=True, text=True, timeout=60,
                           env=dict(os.environ, QT_QPA_PLATFORM="offscreen")).stdout
    have = [l.strip().rstrip("()") for l in funcs.splitlines() if l.strip()]
    report["driver_functions"] = have
    missing = [f for f in fx["flow_functions"] if f not in have]
    if missing:
        report["reasons"].append("the launcher has no prebuilt-install flow: its driver lacks %s (it has %s)" % (", ".join(missing), ", ".join(have)))
        return finish()
    if not os.path.isfile(a.iso):
        report["reasons"].append("no disc image at %s" % a.iso)
        return finish()
    if not os.access(a.game, os.X_OK):
        report["reasons"].append("no game build at %s" % a.game)
        return finish()

    # the cold machine: an empty home with the player's disc in Downloads
    home = tempfile.mkdtemp(prefix="te-launcher-cold-home-")
    downloads = os.path.join(home, "Downloads")
    os.makedirs(downloads)
    real_iso = os.path.realpath(a.iso)
    disc = os.path.join(downloads, os.path.basename(real_iso))
    try:
        os.link(real_iso, disc)   # a hard link: no copy; the original file is only read
    except OSError:
        os.symlink(real_iso, disc)
    report["disc"] = disc

    # the release: one published tag whose asset for this computer is the game build, zipped, with SHA256SUMS
    target = host_target()
    rel_dir = tempfile.mkdtemp(prefix="te-launcher-cold-release-")
    zip_name, zip_path, sha = release_zip(a.game, target, fx["release_tag"], rel_dir)
    sums = os.path.join(rel_dir, "SHA256SUMS")
    with open(sums, "w") as f:
        f.write("%s  ./%s\n" % (sha, zip_name))
    files = {zip_name: zip_path, "SHA256SUMS": sums}
    # the newer launcher (the update-available case): its own release, this computer's launcher file + its SHA256SUMS
    lu = fx.get("launcher_update")
    manifest = json.load(open(a.manifest, encoding="utf-8"))
    lu_asset = ""
    if lu:
        pattern = manifest["release"].get("launcher_assets", {}).get(target, "")
        if not pattern:
            report["reasons"].append("title.json names no launcher file for %s (release.launcher_assets)" % target)
            return finish()
        lu_asset = pattern.replace("{tag}", lu["tag"])
        lu_dir = os.path.join(rel_dir, lu["tag"])
        os.makedirs(lu_dir)
        lu_path = os.path.join(lu_dir, lu_asset)
        with zipfile.ZipFile(lu_path, "w", zipfile.ZIP_DEFLATED) as z:   # never downloaded by a launcher that cannot replace itself
            z.writestr("TwinsanityEvolved.app/Contents/Info.plist", "<plist><dict></dict></plist>\n")
        lu_sums = os.path.join(lu_dir, "SHA256SUMS")
        with open(lu_sums, "w") as f:
            f.write("%s  ./%s\n" % (hashlib.sha256(open(lu_path, "rb").read()).hexdigest(), lu_asset))
        files[lu["tag"] + "/" + lu_asset] = lu_path
        files[lu["tag"] + "/SHA256SUMS"] = lu_sums
    httpd, served = serve(files)
    port = httpd.server_address[1]
    def asset(n, p):
        return {"name": os.path.basename(n), "size": os.path.getsize(p), "browser_download_url": "http://127.0.0.1:%d/download/%s" % (port, n),
                "url": "http://127.0.0.1:%d/download/%s" % (port, n)}
    game_assets = [asset(n, p) for n, p in files.items() if "/" not in n]
    releases = []
    if lu:
        # published now: after the driver was made (its program file's time, as it has no BUILD-ID)
        releases.append({"tag_name": lu["tag"], "draft": False, "prerelease": False,
                         "published_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
                         "html_url": "http://127.0.0.1:%d/releases/tag/%s" % (port, lu["tag"]),
                         "assets": [asset(n, p) for n, p in files.items() if n.startswith(lu["tag"] + "/")]})
    releases.append({"tag_name": fx["release_tag"], "draft": False, "prerelease": False,
                     "published_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(time.time() - 86400)), "assets": game_assets})
    served["releases"] = json.dumps(releases).encode()
    report["launcher_update_asset"] = lu_asset
    releases_url = "http://127.0.0.1:%d/releases" % httpd.server_address[1]
    report["releases_url"] = releases_url
    report["release_asset"] = zip_name
    report["release_asset_bytes"] = os.path.getsize(zip_path)

    # the launcher's settings file: the release server (the one setting a judge needs); nothing else is in HOME
    os.makedirs(app_data(home))
    with open(os.path.join(app_data(home), "launcher.json"), "w") as f:
        json.dump({"releases_url": releases_url}, f)
    titles = os.path.join(rel_dir, "titles")   # the manifest a release ships beside the launcher (outside HOME)
    os.makedirs(os.path.join(titles, "discs"))
    shutil.copy(a.manifest, os.path.join(titles, "title.json"))
    # the disc tables as a release ships them (release_launcher.sh: titles/discs/*.json); launcher/discs/archives/ is the
    # importer's, built into the import library, never beside the launcher
    for d in os.listdir(os.path.join(os.path.dirname(a.manifest), "discs")):
        src = os.path.join(os.path.dirname(a.manifest), "discs", d)
        if os.path.isfile(src):
            shutil.copy(src, os.path.join(titles, "discs", d))

    shots = os.path.join(a.out, "screenshots")
    os.makedirs(shots, exist_ok=True)
    cold_report = os.path.join(a.out, "cold_report.json")
    env = {
        "HOME": home, "USERPROFILE": home, "LOCALAPPDATA": os.path.join(home, "AppData", "Local"),
        "PATH": "/usr/bin:/bin:/usr/sbin:/sbin", "TMPDIR": os.environ.get("TMPDIR", "/tmp"),
        "QT_QPA_PLATFORM": "offscreen", "TE_GFX_HEADLESS": "1",
        "PS2R_COLD_MANIFEST": os.path.join(titles, "title.json"), "PS2R_COLD_REPORT": cold_report, "PS2R_COLD_SCREENSHOTS": shots,
        "PS2R_COLD_INSTALL_TIMEOUT_S": str(fx["install_timeout_s"]), "PS2R_COLD_TITLE_TIMEOUT_S": str(fx["title_timeout_s"]),
        "QTEST_FUNCTION_TIMEOUT": str((fx["install_timeout_s"] + fx["title_timeout_s"] + 300) * 1000),
    }
    print("launcher_cold_judge: HOME=%s  server=%s  driver=%s" % (home, releases_url, driver))
    t0 = time.time()
    with open(os.path.join(a.out, "cold.log"), "w") as log:
        try:
            proc = subprocess.run([driver, "-v1"], env=env, stdout=log, stderr=subprocess.STDOUT,
                                  timeout=fx["max_wall_seconds"] + 120)
            rc = proc.returncode
        except subprocess.TimeoutExpired:
            rc = -9
    wall = time.time() - t0
    httpd.shutdown()
    report["wall_seconds"] = round(wall, 1)
    report["driver_exit"] = rc
    report["served_bytes"] = served["bytes"]
    report["served_requests"] = served["requests"]
    cold = json.load(open(cold_report)) if os.path.isfile(cold_report) else {}
    report["cold"] = cold
    report["download_bytes"] = cold.get("download_bytes")
    report["install_seconds"] = cold.get("install_seconds")
    report["title_seconds"] = cold.get("title_seconds")
    report["screenshots"] = sorted(os.listdir(shots))

    r = report["reasons"]
    if rc != 0:
        r.append("the driver failed (exit %d): see cold.log" % rc)
    if cold.get("commands_typed", 1) != 0:
        r.append("the report does not say zero commands were typed")
    if cold.get("disc_region") != fx["disc_region"]:
        r.append("the disc was identified as %s, not %s" % (cold.get("disc_region"), fx["disc_region"]))
    if cold.get("disc_state") != fx["disc_state"]:
        r.append("the disc's state badge was %r, not %r" % (cold.get("disc_state"), fx["disc_state"]))
    if os.path.normpath(cold.get("game_folder", "")) != os.path.normpath(os.path.join(home, fx["game_folder"])):
        r.append("the game folder was %s, not ~/%s" % (cold.get("game_folder"), fx["game_folder"]))
    listing = set(cold.get("game_folder_listing", []))
    for entry in fx["game_folder_must_hold"]:
        if entry not in listing:
            r.append("the game folder has no %s" % entry)
    for mod_id in fx.get("bundled_mods_must_be_placed", []):
        if mod_id not in cold.get("bundled_mods", []):
            r.append("the release's mod %s was not placed into the mods folder" % mod_id)
    ini = cold.get("ps2r_ini", "")
    for k, v in fx["ps2r_ini_must_set"].items():
        if not any(l.split("=")[0].strip() == k and l.split("=", 1)[1].strip() == v for l in ini.splitlines() if "=" in l):
            r.append("ps2r.ini does not set %s = %s" % (k, v))
    if os.path.realpath(cold.get("disc_image_txt", "")) != os.path.realpath(disc):
        r.append("disc-image.txt names %r, not the chosen disc" % cold.get("disc_image_txt"))
    args = cold.get("play_arguments", [])
    for x in fx["play_arguments_must_include"]:
        if x not in args:
            r.append("Play did not pass %s" % x)
    for x in fx["play_arguments_must_not_include"]:
        if x in args:
            r.append("Play passed %s, which ps2r.ini already says" % x)
    if not cold.get("title_reached"):
        r.append("the game did not reach %s in the launcher's play log" % fx["title_state_name"])
    else:
        log = cold.get("play_log", "")
        if not (os.path.isfile(log) and fx["title_state_name"] in open(log, errors="replace").read()):
            r.append("the play log %s does not show %s" % (log, fx["title_state_name"]))
    if not cold.get("download_bytes"):
        r.append("nothing was downloaded")
    # informational: whether the game read the folder's ps2r.ini (a game build older than the folder keys refuses the
    # whole file and plays with its defaults -- reported, not judged: the launcher's half is the file it wrote)
    play_log = cold.get("play_log", "")
    text = open(play_log, errors="replace").read() if os.path.isfile(play_log) else ""
    report["game_settings_line"] = next((l for l in text.splitlines() if "settings:" in l), "")
    report["game_applied_ps2r_ini"] = "not applied" not in report["game_settings_line"]
    report["game_disc_line"] = next((l for l in text.splitlines() if "] disc:" in l), "")
    # the update-available case: offered, said why it cannot replace itself here, the release's page given, not downloaded
    if lu:
        u = cold.get("launcher_update", {})
        report["launcher_update"] = u
        if not u.get("available"):
            r.append("no newer launcher was offered (the check's log: %s)" % u.get("log"))
        if u.get("tag") != lu["tag"]:
            r.append("the launcher offered was %r, not %r" % (u.get("tag"), lu["tag"]))
        for key in ("games_line", "about_line"):
            if u.get(key) != lu[key]:
                r.append("%s was %r, not %r" % (key, u.get(key), lu[key]))
        if not u.get("games_visible"):
            r.append("the Games page does not show the offer")
        if u.get("games_update_button") != lu["games_update_button"] or u.get("games_download_button") != lu["games_download_button"]:
            r.append("the Games page's buttons: Update %s, Download it %s (want %s, %s)" % (u.get("games_update_button"), u.get("games_download_button"),
                                                                                          lu["games_update_button"], lu["games_download_button"]))
        if not str(u.get("download_page", "")).endswith(lu["download_page_suffix"]):
            r.append("the download page was %r, not the release's (..%s)" % (u.get("download_page"), lu["download_page_suffix"]))
        if u.get("game_update_offered", "") != "":
            r.append("the game's Update was offered for the launcher's release (%s)" % u.get("game_update_offered"))
        fetched = [q for q in served["requests"] if lu_asset in q]
        report["launcher_update_asset_requests"] = fetched
        if fetched and not lu["asset_downloaded"]:
            r.append("the launcher file was downloaded by a launcher that cannot replace itself: %s" % fetched)
    if wall > fx["max_wall_seconds"]:
        r.append("the cold run took %.0f s (limit %d s)" % (wall, fx["max_wall_seconds"]))
    report["result"] = "GREEN" if not r else "RED"
    if not a.keep_home and report["result"] == "GREEN":
        shutil.rmtree(home, ignore_errors=True)
    else:
        report["home"] = home
    shutil.rmtree(rel_dir, ignore_errors=True)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
