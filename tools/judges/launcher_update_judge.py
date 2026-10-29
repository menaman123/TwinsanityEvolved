#!/usr/bin/env python3
"""tools/judges/launcher_update_judge.py -- the launcher replacing ITSELF, end to end on this Mac.

Two real launchers are packaged from one build directory by tools/ci/release_launcher.sh -- the old one (`old_version`)
and the new one (`new_version`), each the signed TwinsanityEvolved.app plus the zip a running launcher downloads to
replace itself (`twinsanityevolved-launcher-<version>-macos-<arch>.zip`) -- with their title.json naming a release server
on 127.0.0.1 (offline-safe: a Python http.server standing in for the GitHub API, newest release first, each with its own
SHA256SUMS). Then, with HOME pointed at a scratch folder (never the user's launcher data):

  1. the OLD launcher is put in <home>/Applications and opened by LaunchServices with --home <home> --update-launcher
     --no-scan (the window's "Update the launcher" without the click; the home already holds a set-up launcher's state,
     so no setup and no disc search runs): it must find the new release by the
     list's order, download it, check it against SHA256SUMS, unpack it beside itself, check the bundle (its id, its
     program, codesign --verify --deep --strict), swap it in, start the waiting relaunch and quit;
  2. the NEW launcher must open by itself (LaunchServices, through the relaunch) -- no prompt, no click -- and remove the
     copy that was set aside, saying "after the relaunch: <new> (recorded <old> -> <new>)" in its update log;
  3. the replaced bundle: the new version (Info.plist and its own log), no quarantine flag on it, codesign valid; spctl's
     verdict recorded (an ad-hoc signature is "rejected" by assessment: what a browser-downloaded copy of the same app
     would meet -- the reason the update must not carry a quarantine flag); the update log's line about the downloaded
     file's quarantine flag recorded;
  4. a launcher in a folder this account cannot change must refuse with the sentence and change nothing;
  5. the user's own launcher data (~/Library/Application Support/<product>) is untouched (its listing and times before
     and after), and no launcher data folder of the judge's own name appeared under the account's home.

The packaged launchers carry a copy of launcher/title.json whose app_data_folder is "<folder>-UpdateJudge"
(release_launcher.sh --manifest): even a launcher that lost its --home could not read or write a player's data.

  tools/judges/launcher_update_judge.py --out DIR [--qt PREFIX] [--work DIR] [--home DIR] [--fixture F] [--keep]

Writes <out>/report.json and the logs; exit 0 GREEN, 1 RED. Frozen: fixtures/judges/launcher_update_pal.json.

How a launcher is opened: by a one-file STARTER app (compiled here, ad-hoc signed) that LaunchServices opens and that
itself runs `open -n <launcher> --args ...` -- the launcher's arguments travel through LaunchServices as a player's would,
and a caller whose own `open` drops arguments (a sandboxed shell: seen in the session that wrote this judge -- arguments
and --env both lost, so the launcher ran with the account's HOME) cannot put a launcher outside the scratch home.
"""
import argparse
import hashlib
import http.server
import json
import os
import platform
import plistlib
import re
import shutil
import signal
import socketserver
import stat
import subprocess
import sys
import tempfile
import threading
import time
import uuid

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))


class Server(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True


def serve():
    state = {"releases": b"[]", "files": {}, "requests": [], "bytes": 0}

    class Handler(http.server.BaseHTTPRequestHandler):
        def log_message(self, *args):
            pass

        def do_GET(self):
            state["requests"].append(self.path)
            path = self.path.split("?")[0]
            if path == "/releases":
                body, ctype = state["releases"], "application/json"
            elif path.startswith("/download/") and path[len("/download/"):] in state["files"]:
                with open(state["files"][path[len("/download/"):]], "rb") as f:
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


def run(cmd, log, env=None, timeout=3600):
    with open(log, "w") as f:
        f.write("$ %s\n" % " ".join(cmd))
        f.flush()
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, env=env, timeout=timeout, cwd=ROOT)
    return p.returncode


def text(cmd):
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p.returncode, (p.stdout + p.stderr).strip()


def plist_version(app):
    try:
        with open(os.path.join(app, "Contents", "Info.plist"), "rb") as f:
            return plistlib.load(f).get("CFBundleShortVersionString", "")
    except (OSError, plistlib.InvalidFileException):
        return ""


def update_logs(home, product):
    d = os.path.join(home, "Library", "Application Support", product, "Logs")
    out = []
    for n in sorted(os.listdir(d)) if os.path.isdir(d) else []:
        if n.startswith("launcher-update-"):
            out.append(os.path.join(d, n))
    return out


def all_log_text(home, product):
    return "".join(open(p, errors="replace").read() for p in update_logs(home, product))


def snapshot(path):
    """the user's launcher data, by stat only (never listed or opened): the folder, its state file and its Logs and
    Downloads folders -- a folder's time changes when anything is added to it or removed"""
    out = {}
    for rel in (".", "launcher.json", "Logs", "Downloads"):
        try:
            st = os.stat(os.path.join(path, rel))
            out[rel] = (st.st_size, st.st_mtime_ns, st.st_ino)
        except OSError:
            out[rel] = None
    return out


STARTER_C = r"""
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <mach-o/dyld.h>
/* opens what Contents/Resources/command lists (one argument a line) with /usr/bin/open, then exits */
int main(void) {
    char exe[4096]; uint32_t n = sizeof(exe);
    if (_NSGetExecutablePath(exe, &n) != 0) return 2;
    char* slash = strrchr(exe, '/'); if (!slash) return 2; *slash = 0;          /* .../Contents/MacOS */
    char path[4300]; snprintf(path, sizeof(path), "%s/../Resources/command", exe);
    FILE* f = fopen(path, "r"); if (!f) return 3;
    static char lines[64][4096]; char* argv[66]; int argc = 0;
    argv[argc++] = "open";
    while (argc < 65 && fgets(lines[argc], sizeof(lines[argc]), f)) {
        lines[argc][strcspn(lines[argc], "\n")] = 0; argv[argc] = lines[argc]; argc++;
    }
    fclose(f); argv[argc] = 0;
    execv("/usr/bin/open", argv);
    return 4;
}
"""


def make_starter(work):
    app = os.path.join(work, "starter", "Starter.app")
    shutil.rmtree(os.path.dirname(app), ignore_errors=True)
    os.makedirs(os.path.join(app, "Contents", "MacOS"))
    os.makedirs(os.path.join(app, "Contents", "Resources"))
    src = os.path.join(work, "starter", "starter.c")
    open(src, "w").write(STARTER_C)
    subprocess.run(["cc", "-O2", "-o", os.path.join(app, "Contents", "MacOS", "starter"), src], check=True)
    with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump({"CFBundleExecutable": "starter", "CFBundleIdentifier": "dev.launcher-update-judge.starter",
                       "CFBundlePackageType": "APPL", "LSUIElement": True}, f)
    subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)
    return app


PROBE_C = r"""
#include <stdio.h>
#include <stdlib.h>
/* writes its arguments, one a line, to the file its first argument names */
int main(int argc, char** argv) {
    if (argc < 2) return 2;
    FILE* f = fopen(argv[1], "w"); if (!f) return 3;
    for (int i = 2; i < argc; ++i) fprintf(f, "%s\n", argv[i]);
    fclose(f);
    return 0;
}
"""


def probe_arguments(work, starter):
    """True when an app the starter opens receives its arguments (else no launcher is opened: it could run in the
    account's own HOME)"""
    app = os.path.join(work, "starter", "Probe.app")
    os.makedirs(os.path.join(app, "Contents", "MacOS"), exist_ok=True)
    src = os.path.join(work, "starter", "probe.c")
    open(src, "w").write(PROBE_C)
    subprocess.run(["cc", "-O2", "-o", os.path.join(app, "Contents", "MacOS", "probe"), src], check=True)
    with open(os.path.join(app, "Contents", "Info.plist"), "wb") as f:
        plistlib.dump({"CFBundleExecutable": "probe", "CFBundleIdentifier": "dev.launcher-update-judge.probe",
                       "CFBundlePackageType": "APPL", "LSUIElement": True}, f)
    subprocess.run(["codesign", "--force", "--sign", "-", app], check=True, capture_output=True)
    out = os.path.join(work, "starter", "probe.out")
    if os.path.exists(out):
        os.remove(out)
    launch(starter, app, [out, "--home", "/tmp/a b"])
    t0 = time.time()
    while time.time() - t0 < 20 and not os.path.exists(out):
        time.sleep(0.2)
    time.sleep(0.3)
    return os.path.exists(out) and open(out).read().split("\n")[:2] == ["--home", "/tmp/a b"]


def launch(starter, app, args):
    """LaunchServices opens the starter, the starter opens `app` with `args` (see the module's note)"""
    with open(os.path.join(starter, "Contents", "Resources", "command"), "w") as f:
        f.write("\n".join(["-n", app, "--args"] + args) + "\n")
    return text(["open", "-n", starter])


def seed_home(home, product):
    """a launcher that was set up before: an install of the game for this computer (a folder with its settings file), so
    the window opens on the Games page -- no setup sheet, no disc search, no disc read"""
    folder = os.path.join(home, "Games", "Crash Twinsanity")
    os.makedirs(folder, exist_ok=True)
    open(os.path.join(folder, "ps2r.ini"), "w").write("# ps2r.ini -- the judge's game folder\n")
    data = os.path.join(home, "Library", "Application Support", product)
    os.makedirs(data, exist_ok=True)
    target = "macos-arm64" if platform.machine() == "arm64" else "macos-x86_64"
    json.dump({"installs": [{"target": target, "folder": folder, "tag": "v0.0.1", "placed": os.path.join(folder, "twinsanityevolved"),
                             "at": "2026-10-28T00:00:00"}], "chosen_folder": folder}, open(os.path.join(data, "launcher.json"), "w"), indent=2)


def alive(pid):
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


def stop(pid, timeout=20):
    if not pid or not alive(pid):
        return True
    os.kill(pid, signal.SIGTERM)
    t0 = time.time()
    while time.time() - t0 < timeout:
        if not alive(pid):
            return True
        time.sleep(0.2)
    os.kill(pid, signal.SIGKILL)
    return not alive(pid)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", required=True)
    ap.add_argument("--qt", default=os.environ.get("QT_PREFIX", "/opt/homebrew/opt/qt@6"))
    ap.add_argument("--work", help="where the two launchers are packaged (default: <out>/work; its build/ is reused)")
    ap.add_argument("--home", help="the scratch HOME (default: a new temporary folder); never the user's own")
    ap.add_argument("--fixture", default=os.path.join(ROOT, "fixtures", "judges", "launcher_update_pal.json"))
    ap.add_argument("--keep", action="store_true", help="keep the packaged launchers and the home")
    a = ap.parse_args()
    fx = json.load(open(a.fixture))
    os.makedirs(a.out, exist_ok=True)
    report = {"judge": "launcher_update_judge", "result": "RED", "reasons": [], "fixture": os.path.relpath(a.fixture, ROOT)}
    r = report["reasons"]

    def finish():
        json.dump(report, open(os.path.join(a.out, "report.json"), "w"), indent=2)
        print("launcher_update_judge: %s -- %s" % (report["result"], "; ".join(r) or "every step held"))
        return 0 if report["result"] == "GREEN" else 1

    if platform.system() != "Darwin":
        r.append("macOS only (the Windows and Linux replacements are the unit test's: LauncherSelfUpdate)")
        return finish()
    manifest = json.load(open(os.path.join(ROOT, "launcher", "title.json"), encoding="utf-8"))
    product = manifest["product"]
    # the packaged launchers carry a copy of the manifest naming their OWN data folder: a launcher this judge opens can
    # never read or write a player's launcher data, even if it were started with the account's own home folder
    real_data_name = manifest.get("app_data_folder", product)
    data_name = real_data_name + "-UpdateJudge"
    report["data_folder_name"] = data_name
    arch = platform.machine()
    target = "macos-arm64" if arch == "arm64" else "macos-x86_64"
    pattern = manifest["release"].get("launcher_assets", {}).get(target, "")
    if not pattern:
        r.append("title.json names no launcher file for %s" % target)
        return finish()
    old, new = fx["old_version"], fx["new_version"]
    work = os.path.abspath(a.work or os.path.join(a.out, "work"))
    os.makedirs(work, exist_ok=True)
    judge_manifest = os.path.join(work, "title.json")
    json.dump(dict(manifest, app_data_folder=data_name), open(judge_manifest, "w", encoding="utf-8"), indent=2)
    home = os.path.abspath(a.home) if a.home else tempfile.mkdtemp(prefix="te-launcher-update-home-")
    user_data = os.path.join(os.path.expanduser("~"), "Library", "Application Support", real_data_name)
    stray = os.path.join(os.path.expanduser("~"), "Library", "Application Support", data_name)
    report["stray_data_before"] = os.path.exists(stray)
    if os.path.realpath(home) == os.path.realpath(os.path.expanduser("~")):
        r.append("--home is the user's own home: refused")
        return finish()
    if os.path.exists(home):
        shutil.rmtree(home)
    os.makedirs(os.path.join(home, "Applications"))
    report["home"] = home
    before = snapshot(user_data)

    # the release server first: both launchers' title.json name it
    httpd, served = serve()
    port = httpd.server_address[1]
    releases_url = "http://127.0.0.1:%d/releases" % port
    report["releases_url"] = releases_url

    # 1. the two launchers, from one build directory (the version is compiled into one source file)
    env = dict(os.environ, TE_MIN_FREE_GB=str(fx["min_free_gb"]))
    built = {}
    for version, folder in ((old, "old"), (new, "new")):
        dist = os.path.join(work, folder)
        shutil.rmtree(dist, ignore_errors=True)
        t0 = time.time()
        rc = run(["bash", "tools/ci/release_launcher.sh", "--qt", a.qt, "--build-dir", os.path.join(work, "build"), "--dist-dir", dist,
                  "--version", version, "--releases-url", releases_url, "--no-dmg", "--manifest", judge_manifest],
                 os.path.join(a.out, "package-%s.log" % folder), env=env)
        app = os.path.join(dist, "launcher-macos-%s" % arch, product + ".app")
        zname = pattern.replace("{tag}", version)
        zpath = os.path.join(dist, zname)
        built[version] = {"app": app, "zip": zpath, "zip_name": zname, "seconds": round(time.time() - t0, 1), "exit": rc}
        if rc != 0 or not os.path.isdir(app) or not os.path.isfile(zpath):
            r.append("release_launcher.sh did not package %s (exit %d): see package-%s.log" % (version, rc, folder))
            report["packaged"] = built
            httpd.shutdown()
            return finish()
        built[version]["plist_version"] = plist_version(app)
        built[version]["zip_bytes"] = os.path.getsize(zpath)
    report["packaged"] = built

    # the releases: the new one on top (newest first), each with its own SHA256SUMS
    rel = []
    now = time.time()
    for i, version in enumerate((new, old)):
        b = built[version]
        sums = os.path.join(work, "SHA256SUMS-" + version)
        with open(sums, "w") as f:
            f.write("%s  ./%s\n" % (hashlib.sha256(open(b["zip"], "rb").read()).hexdigest(), b["zip_name"]))
        served["files"]["%s/%s" % (version, b["zip_name"])] = b["zip"]
        served["files"]["%s/SHA256SUMS" % version] = sums
        rel.append({"tag_name": version, "draft": False, "prerelease": False,
                    "published_at": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime(now - 86400 * 7 * i)),
                    "html_url": "http://127.0.0.1:%d/releases/tag/%s" % (port, version),
                    "assets": [{"name": n, "size": os.path.getsize(p), "browser_download_url": "http://127.0.0.1:%d/download/%s/%s" % (port, version, n),
                                "url": "http://127.0.0.1:%d/download/%s/%s" % (port, version, n)}
                               for n, p in ((b["zip_name"], b["zip"]), ("SHA256SUMS", sums))]})
    served["releases"] = json.dumps(rel).encode()

    # 2. the old launcher installed and opened as a player opens it (LaunchServices), told to update itself
    installed = os.path.join(home, "Applications", product + ".app")
    subprocess.run(["ditto", built[old]["app"], installed], check=True)
    seed_home(home, data_name)
    starter = make_starter(work)
    report["starter_passes_arguments"] = probe_arguments(work, starter)
    if not report["starter_passes_arguments"]:
        r.append("an app opened by the starter did not receive its arguments: no launcher is opened (it would run in the account's HOME)")
        httpd.shutdown()
        return finish()
    report["installed_version_before"] = plist_version(installed)
    report["installed_quarantine_before"] = text(["xattr", "-p", "com.apple.quarantine", installed])[1]
    t0 = time.time()
    rc, said = launch(starter, installed, ["--home", home, "--update-launcher", "--no-scan"])
    report["open_old"] = {"exit": rc, "said": said}
    # the old launcher must be in the scratch home: its first log line names it (else stop it at once)
    t_first = time.time()
    while time.time() - t_first < 30 and not update_logs(home, data_name):
        time.sleep(0.25)
    if not update_logs(home, data_name):
        r.append("the old launcher wrote no update log in the scratch home within 30 s: stopped")
        for line in subprocess.run(["pgrep", "-f", installed + "/Contents/MacOS/"], capture_output=True, text=True).stdout.split():
            stop(int(line))
        httpd.shutdown()
        report["user_data_untouched"] = before == snapshot(user_data)
        return finish()
    new_pid = 0
    done = False
    while time.time() - t0 < fx["timeout_s"]:
        logs = all_log_text(home, data_name)
        m = re.search(r"after the relaunch: (\S+) \(recorded (\S+) -> (\S+)\), process (\d+)", logs)
        state = os.path.join(home, "Library", "Application Support", data_name, "launcher.json")
        pending = os.path.isfile(state) and "launcher_update" in json.load(open(state))
        if m and not pending and "the replaced copy was removed" in logs:
            new_pid = int(m.group(4))
            done = True
            break
        time.sleep(0.5)
    report["update_seconds"] = round(time.time() - t0, 1)
    time.sleep(1.0)
    logs = all_log_text(home, data_name)
    report["update_logs"] = update_logs(home, data_name)
    for i, p in enumerate(report["update_logs"]):
        shutil.copy(p, os.path.join(a.out, "update-log-%d.txt" % i))
    report["served_requests"] = served["requests"]
    if not done:
        r.append("the new launcher did not open and finish within %d s (see update-log-*.txt)" % fx["timeout_s"])
    for line in fx["must_log"]:
        if line.replace("{old}", old).replace("{new}", new) not in logs:
            r.append("the update logs do not say %r" % line.replace("{old}", old).replace("{new}", new))
    m = re.search(r"download: quarantine flag (\S+)", logs)
    report["download_quarantine"] = m.group(1) if m else None
    m = re.search(r"unpacked: .*; quarantine flag (\S+)", logs)
    report["unpacked_quarantine"] = m.group(1) if m else None
    m = re.search(r"after the relaunch: .* HOME (.+)", logs)
    report["relaunched_home"] = m.group(1).strip() if m else None
    same_home = bool(report["relaunched_home"]) and os.path.isdir(report["relaunched_home"]) and os.path.samefile(report["relaunched_home"], home)
    if not same_home:
        r.append("the relaunched launcher's HOME was %r, not the scratch home" % report["relaunched_home"])

    # 3. the replaced bundle
    report["installed_version_after"] = plist_version(installed)
    if report["installed_version_after"] != new:
        r.append("the bundle at %s is %r after the update, not %s" % (installed, report["installed_version_after"], new))
    listing = sorted(os.listdir(os.path.join(home, "Applications")))
    report["applications_after"] = listing
    if listing != [product + ".app"]:
        r.append("the Applications folder holds %s after the update (the copy set aside must be gone)" % listing)
    rc, xattrs = text(["xattr", "-lr", installed])
    open(os.path.join(a.out, "xattr-after.txt"), "w").write(xattrs + "\n")
    report["quarantine_on_new_bundle"] = "com.apple.quarantine" in xattrs
    if report["quarantine_on_new_bundle"]:
        r.append("the replaced bundle carries a quarantine flag (xattr-after.txt)")
    report["xattr_names_after"] = sorted(set(re.findall(r": (com\.apple\.[A-Za-z.]+):", xattrs)))
    rc, said = text(["codesign", "--verify", "--deep", "--strict", "--verbose=2", installed])
    report["codesign_verify"] = {"exit": rc, "said": said}
    if rc != 0:
        r.append("codesign --verify of the replaced bundle failed: %s" % said)
    report["codesign_display"] = text(["codesign", "-dv", "--verbose=2", installed])[1]
    report["spctl_assess"] = dict(zip(("exit", "said"), text(["spctl", "--assess", "--type", "execute", "-vv", installed])))
    report["new_pid"] = new_pid
    report["new_running_after"] = alive(new_pid) if new_pid else False
    if new_pid and not report["new_running_after"]:
        r.append("the new launcher (process %d) is not running after the update" % new_pid)
    stop(new_pid)

    # 3b. the same bundle as a browser would have left it (quarantined, never approved): spctl's verdict, not opened
    q = os.path.join(work, "quarantined", product + ".app")
    shutil.rmtree(os.path.dirname(q), ignore_errors=True)
    os.makedirs(os.path.dirname(q))
    subprocess.run(["ditto", installed, q], check=True)
    subprocess.run(["xattr", "-w", "com.apple.quarantine", "0083;%08x;Safari;%s" % (int(time.time()), str(uuid.uuid4()).upper()), q], check=True)
    report["spctl_assess_if_quarantined"] = dict(zip(("exit", "said"), text(["spctl", "--assess", "--type", "execute", "-vv", q])))
    shutil.rmtree(os.path.dirname(q), ignore_errors=True)

    # 4. a launcher in a folder this account cannot change: the sentence, nothing changed
    locked = os.path.join(home, "Locked")
    os.makedirs(locked)
    locked_app = os.path.join(locked, product + ".app")
    subprocess.run(["ditto", built[old]["app"], locked_app], check=True)
    os.chmod(locked, stat.S_IRUSR | stat.S_IXUSR)
    n_logs = len(update_logs(home, data_name))
    t0 = time.time()
    launch(starter, locked_app, ["--home", home, "--update-launcher", "--no-scan"])
    locked_pid, refused = 0, ""
    while time.time() - t0 < 60:
        fresh = update_logs(home, data_name)[n_logs:]
        body = "".join(open(p, errors="replace").read() for p in fresh)
        m = re.search(r"failed: (.*)", body)
        p = re.search(r"check: \S+ \(.*process (\d+)\)", body)
        if p:
            locked_pid = int(p.group(1))
        if m:
            refused = m.group(1).strip()
            break
        time.sleep(0.5)
    stop(locked_pid)
    os.chmod(locked, stat.S_IRWXU)
    report["refusal"] = {"said": refused, "version_after": plist_version(locked_app), "pid": locked_pid}
    want = fx["refusal_line"].replace("{home_locked}", "~" + locked[len(home):] if locked.startswith(home) else locked)
    if not refused.startswith(want):
        r.append("the locked launcher said %r, not %r..." % (refused, want))
    if plist_version(locked_app) != old:
        r.append("the locked launcher changed (%s)" % plist_version(locked_app))

    # 5. the user's own launcher data
    after = snapshot(user_data)
    report["user_data"] = user_data
    report["user_data_untouched"] = before == after
    if before != after:
        r.append("%s changed during the judge" % user_data)
    report["stray_data_after"] = os.path.exists(stray)
    if report["stray_data_after"] and not report["stray_data_before"]:
        r.append("%s appeared: a launcher ran with the account's own home folder" % stray)

    httpd.shutdown()
    report["result"] = "GREEN" if not r else "RED"
    if not a.keep and report["result"] == "GREEN":
        shutil.rmtree(home, ignore_errors=True)
        for folder in ("old", "new"):
            shutil.rmtree(os.path.join(work, folder), ignore_errors=True)
    return finish()


if __name__ == "__main__":
    sys.exit(main())
