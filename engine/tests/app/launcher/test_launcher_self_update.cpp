// tests/app/launcher/test_launcher_self_update.cpp -- the launcher's own update without a
// window: which release is newer (the list's order, the dates, development versions, a computer without a launcher
// file), the file steps (the swap and its undo, the folder's set-aside files and their undo), then the whole update
// against FixtureReleaseServer serving TWO launcher versions -- a macOS bundle (a real program, ad-hoc signed, zipped,
// checked by codesign after the unpack), a Windows-style folder and an AppImage file -- each replaced in a temporary
// folder, the relaunch recorded instead of run, and the next launcher's start removing what was set aside. The
// refusals: not writable, not installed from a release, the game running, a damaged download, another program's zip.
// A file:// list (the hand-test kit's way of serving releases) updates the same way; a size the file does not have is refused.
// The real relaunch (two built launchers, LaunchServices, Gatekeeper) is tools/judges/launcher_update_judge.py's.
#include <QtTest/QtTest>
#include <QCryptographicHash>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSignalSpy>
#include <QTemporaryDir>
#ifndef Q_OS_WIN
#include <csignal>
#include <sys/types.h>
#endif
#if __has_include(<QtCore/private/qzipwriter_p.h>)
#include <QtCore/private/qzipwriter_p.h>
#else
#include <QtGui/private/qzipwriter_p.h>
#endif
#include "app_data.h"
#include "fixture_release_server.h"
#include "release_catalogue.h"
#include "self_update.h"
#include "title_manifest.h"
using namespace ps2r::launcher;

namespace {
const char* kManifest = R"JSON({
  "schema": 2, "id": "SLES-52568", "name": "Crash Twinsanity", "product": "TwinsanityEvolved", "app_data_folder": "TwinsanityEvolved",
  "binary_name": "twinsanityevolved",
  "release": { "releases_url": "https:\/\/example.invalid/releases", "page_url": "https:\/\/example.invalid/project",
               "zip_asset": "twinsanityevolved-{tag}-{target}.zip", "vita_asset": "twinsanityevolved-{tag}-vita.vpk", "sums_asset": "SHA256SUMS",
               "launcher_assets": { "macos-arm64": "twinsanityevolved-launcher-{tag}-macos-arm64.zip",
                                    "macos-x86_64": "twinsanityevolved-launcher-{tag}-macos-x86_64.zip",
                                    "windows-x64": "twinsanityevolved-launcher-windows-x64.zip",
                                    "linux-x86_64": "twinsanityevolved-launcher-linux-x86_64.AppImage",
                                    "linux-aarch64": "twinsanityevolved-launcher-linux-aarch64.AppImage" } },
  "targets": { "computers": [ { "id": "macos-arm64", "label": "macOS (Apple silicon)", "exe": "twinsanityevolved" } ] }
})JSON";

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
[[maybe_unused]] QByteArray infoPlist(const QString& id, const QString& program, const QString& version) {
    return QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<plist version=\"1.0\">\n<dict>\n"
                          "\t<key>CFBundleExecutable</key>\n\t<string>%2</string>\n"
                          "\t<key>CFBundleIdentifier</key>\n\t<string>%1</string>\n"
                          "\t<key>CFBundleShortVersionString</key>\n\t<string>%3</string>\n"
                          "\t<key>CFBundlePackageType</key>\n\t<string>APPL</string>\n</dict>\n</plist>\n")
        .arg(id, program, version).toUtf8();
}
// a zip of `dir` (its name the zip's top entry, as `ditto -c -k --keepParent` makes it), permissions and links kept
QByteArray zipTree(const QString& dir) {
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QZipWriter zip(&buf);
    const QString top = QFileInfo(dir).fileName();
    zip.addDirectory(top);
    QDirIterator it(dir, QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    QStringList paths;
    while (it.hasNext()) paths << it.next();
    paths.sort();
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        const QString rel = top + "/" + QDir(dir).relativeFilePath(p);
        if (fi.isSymLink()) { zip.addSymLink(rel, fi.readSymLink()); continue; }
        if (fi.isDir()) { zip.addDirectory(rel); continue; }
        zip.setCreationPermissions(QFile::permissions(p));
        zip.addFile(rel, readFile(p));
    }
    zip.close();
    return buf.data();
}
QJsonArray releasesOf(const QString& json) { return QJsonDocument::fromJson(json.toUtf8()).array(); }
}  // namespace

class TestLauncherSelfUpdate : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    TitleManifest manifest_;

    RunningLauncher runningAt(const QString& version, const QString& kind, const QString& path, const QString& program) {
        RunningLauncher r;
        r.version = version;
        r.made = QDateTime::currentDateTimeUtc().addDays(-10);
        r.target = ReleaseCatalogue::hostTarget();
        r.kind = kind;
        r.path = path;
        r.program = program;
        r.bundleId = QStringLiteral("dev.nativeee.launcher.sles-52568");
        r.home = home_.path();
        r.pid = 4242;
        return r;
    }

private slots:
    void initTestCase() {
        qputenv("HOME", home_.path().toUtf8());
        qputenv("USERPROFILE", home_.path().toUtf8());
        qputenv("LOCALAPPDATA", home_.path().toUtf8());
        qunsetenv("GITHUB_TOKEN");
        qunsetenv("GH_TOKEN");
        QString err;
        QVERIFY2(manifest_.loadJson(QJsonDocument::fromJson(kManifest).object(), &err, home_.path()), qPrintable(err));
        QCOMPARE(manifest_.launcherAssetPattern("windows-x64"), QStringLiteral("twinsanityevolved-launcher-windows-x64.zip"));
        QCOMPARE(manifest_.launcherAssetPattern("macos-arm64"), QStringLiteral("twinsanityevolved-launcher-{tag}-macos-arm64.zip"));
        QVERIFY(manifest_.launcherAssetPattern("vita").isEmpty());
    }

    // "v0.2.1-14-g1a2b3c4" is 14 commits after v0.2.1; a bare hash has no tag
    void versions() {
        int n = 99;
        QCOMPARE(SelfUpdate::baseTag("v0.2.1", &n), QStringLiteral("v0.2.1"));
        QCOMPARE(n, 0);
        QCOMPARE(SelfUpdate::baseTag("v0.1.8-159-ge58a426", &n), QStringLiteral("v0.1.8"));
        QCOMPARE(n, 159);
        QCOMPARE(SelfUpdate::baseTag("v0.1.8-159-ge58a426-dirty", &n), QStringLiteral("v0.1.8"));
        QCOMPARE(SelfUpdate::baseTag("v1.0.0-rc1", &n), QStringLiteral("v1.0.0-rc1"));   // a tag with a dash stays a tag
        QCOMPARE(n, 0);
        QCOMPARE(SelfUpdate::baseTag("e58a426", &n), QString());
        QCOMPARE(n, -1);
    }

    // newer by the list's order (the API's: newest first) when the running version's tag is in it, else by the dates;
    // never by comparing the version texts
    void newerRule() {
        const QString t = ReleaseCatalogue::hostTarget();
        const QString pattern = manifest_.launcherAssetPattern(t);
        const QString a1 = ReleaseCatalogue::assetName(pattern, "v0.2.1", t), a0 = ReleaseCatalogue::assetName(pattern, "v0.2.0", t);
        const QString aOld = ReleaseCatalogue::assetName(pattern, "v0.10.0", t);
        // v0.10.0 is OLDER here although it sorts after v0.2.1 as text and as numbers: the list's order decides
        const QJsonArray list = releasesOf(QStringLiteral(R"([
          {"tag_name": "v0.3.0-rc1", "prerelease": true, "published_at": "2026-10-28T10:00:00Z", "assets": [{"name": "%4", "browser_download_url": "http:\/\/x/rc"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/srcs"}]},
          {"tag_name": "v0.2.2", "published_at": "2026-10-27T10:00:00Z", "assets": [{"name": "other.zip", "browser_download_url": "http:\/\/x/o"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s22"}]},
          {"tag_name": "v0.2.1", "published_at": "2026-10-26T10:00:00Z", "html_url": "http:\/\/x/page/v0.2.1", "assets": [{"name": "%1", "size": 99, "browser_download_url": "http:\/\/x/l1", "url": "http:\/\/api/l1"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s1", "url": "http:\/\/api/s1"}]},
          {"tag_name": "v0.2.0", "published_at": "2026-10-20T10:00:00Z", "assets": [{"name": "%2", "browser_download_url": "http:\/\/x/l0"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s0"}]},
          {"tag_name": "v0.10.0", "published_at": "2026-10-01T10:00:00Z", "assets": [{"name": "%3", "browser_download_url": "http:\/\/x/lo"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/so"}]}])")
                                                .arg(a1, a0, aOld, ReleaseCatalogue::assetName(pattern, "v0.3.0-rc1", t)));
        LauncherRelease r;
        QString why;
        RunningLauncher run = runningAt("v0.2.0", "app", "/x", "p");
        QVERIFY2(SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r, &why), qPrintable(why));
        QCOMPARE(r.tag, QStringLiteral("v0.2.1"));   // the prerelease and the release without a launcher file are skipped
        QCOMPARE(r.asset.url, QUrl("http://x/l1"));
        QCOMPARE(r.asset.bytes, qint64(99));
        QCOMPARE(r.page, QUrl("http://x/page/v0.2.1"));
        QCOMPARE(r.published, QDateTime(QDate(2026, 10, 26), QTime(10, 0), QTimeZone::UTC));
        QVERIFY(SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, true, &r));
        QCOMPARE(r.asset.url, QUrl("http://api/l1"));   // a token: the API's asset URL
        run.version = "v0.10.0";   // listed after v0.2.1: v0.2.1 is newer
        QVERIFY(SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r));
        run.version = "v0.2.1";    // itself
        QVERIFY(!SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r, &why));
        run.version = "v0.2.2";    // newer than every release carrying a launcher file
        QVERIFY(!SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r));
        run.version = "v0.2.1-14-g1a2b3c4";   // made after v0.2.1: nothing newer
        QVERIFY(!SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r));
        run.version = "v0.2.0-3-gabcdef1";    // made after v0.2.0, before v0.2.1 was listed: v0.2.1 is offered
        QVERIFY(SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r));
        QCOMPARE(r.tag, QStringLiteral("v0.2.1"));
        // a version the list does not name: the dates decide (published after the launcher was made)
        run.version = "e58a426";
        run.made = QDateTime(QDate(2026, 10, 25), QTime(0, 0), QTimeZone::UTC);
        QVERIFY2(SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r, &why), qPrintable(why));
        QCOMPARE(r.tag, QStringLiteral("v0.2.1"));
        run.made = QDateTime(QDate(2026, 10, 26), QTime(10, 0), QTimeZone::UTC);   // the same moment: not newer
        QVERIFY(!SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r));
        run.made = QDateTime();
        QVERIFY(!SelfUpdate::newer(list, pattern, t, "SHA256SUMS", run, false, &r, &why));
        // a computer without a launcher file in title.json, and a list without any
        QVERIFY(!SelfUpdate::newer(list, QString(), t, "SHA256SUMS", runningAt("v0.2.0", "app", "/x", "p"), false, &r, &why));
        QVERIFY(why.contains("no launcher file"));
        QVERIFY(!SelfUpdate::newer(QJsonArray(), pattern, t, "SHA256SUMS", runningAt("v0.2.0", "app", "/x", "p"), false, &r));
        // a hand-made list naming describe strings (the hand-test kit's): the running version itself is found
        const QJsonArray kit = releasesOf(QStringLiteral(R"([
          {"tag_name": "v0.1.8-170-gaaaaaaa", "published_at": "2026-10-29T10:00:00Z", "assets": [{"name": "%1", "browser_download_url": "file:\/\/\/k/l"}, {"name": "SHA256SUMS", "browser_download_url": "file:\/\/\/k/s"}]},
          {"tag_name": "v0.1.8-160-gbbbbbbb", "published_at": "2026-10-28T10:00:00Z", "assets": [{"name": "%2", "browser_download_url": "file:\/\/\/k/l0"}, {"name": "SHA256SUMS", "browser_download_url": "file:\/\/\/k/s0"}]}])")
                                               .arg(ReleaseCatalogue::assetName(pattern, "v0.1.8-170-gaaaaaaa", t), ReleaseCatalogue::assetName(pattern, "v0.1.8-160-gbbbbbbb", t)));
        run = runningAt("v0.1.8-160-gbbbbbbb", "app", "/x", "p");
        QVERIFY(SelfUpdate::newer(kit, pattern, t, "SHA256SUMS", run, false, &r));
        QCOMPARE(r.tag, QStringLiteral("v0.1.8-170-gaaaaaaa"));
        run.version = "v0.1.8-170-gaaaaaaa";
        QVERIFY(!SelfUpdate::newer(kit, pattern, t, "SHA256SUMS", run, false, &r));
    }

    // the swap: the running copy aside, the new one in; undone when the second rename cannot happen
    void swapAndUndo() {
        QTemporaryDir d;
        const QString cur = d.path() + "/X.app", staged = d.path() + "/.X-update/X.app", aside = d.path() + "/.X-replaced-v1.app";
        QVERIFY(writeFile(cur + "/Contents/a", "old"));
        QVERIFY(writeFile(staged + "/Contents/a", "new"));
        QString err;
        QVERIFY2(SelfUpdate::swapIn(cur, staged, aside, &err), qPrintable(err));
        QCOMPARE(readFile(cur + "/Contents/a"), QByteArray("new"));
        QCOMPARE(readFile(aside + "/Contents/a"), QByteArray("old"));
        QVERIFY(!QFileInfo::exists(staged));
        // the new copy is gone before the second rename: the running copy is put back
        QVERIFY(QDir(aside).removeRecursively());
        QVERIFY(!SelfUpdate::swapIn(cur, d.path() + "/missing.app", aside, &err));
        QVERIFY(!err.isEmpty());
        QCOMPARE(readFile(cur + "/Contents/a"), QByteArray("new"));
        QVERIFY(!QFileInfo::exists(aside));
        // the folder's way: every file that is there is renamed aside first; a failure undoes everything done
        const QString into = d.path() + "/Launcher", from = d.path() + "/New";
        QVERIFY(writeFile(into + "/Launcher.exe", "old exe"));
        QVERIFY(writeFile(into + "/Qt6Core.dll", "old core"));
        QVERIFY(writeFile(into + "/keep.txt", "the user's"));
        QVERIFY(writeFile(from + "/Launcher.exe", "new exe"));
        QVERIFY(writeFile(from + "/Qt6Core.dll", "new core"));
        QVERIFY(writeFile(from + "/plugins/platforms/qwindows.dll", "new plugin"));
        QStringList setAside;
        QVERIFY2(SelfUpdate::replaceFiles(from, into, ".replaced", &setAside, &err), qPrintable(err));
        setAside.sort();
        QCOMPARE(setAside, (QStringList{into + "/Launcher.exe.replaced", into + "/Qt6Core.dll.replaced"}));
        QCOMPARE(readFile(into + "/Launcher.exe"), QByteArray("new exe"));
        QCOMPARE(readFile(into + "/Launcher.exe.replaced"), QByteArray("old exe"));
        QCOMPARE(readFile(into + "/plugins/platforms/qwindows.dll"), QByteArray("new plugin"));
        QCOMPARE(readFile(into + "/keep.txt"), QByteArray("the user's"));
#ifndef Q_OS_WIN
        // a folder that cannot be written: nothing of the new copy stays, the old files are back
        QVERIFY(writeFile(from + "/Launcher.exe", "newer exe"));
        QVERIFY(writeFile(from + "/zzz/locked.txt", "x"));
        QVERIFY(QDir().mkpath(into + "/zzz"));
        QVERIFY(QFile::setPermissions(into + "/zzz", QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        setAside.clear();
        QVERIFY(!SelfUpdate::replaceFiles(from, into, ".replaced2", &setAside, &err));
        QVERIFY(QFile::setPermissions(into + "/zzz", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
        QCOMPARE(readFile(into + "/Launcher.exe"), QByteArray("new exe"));   // undone
        QVERIFY(!QFileInfo::exists(into + "/Launcher.exe.replaced2"));
        QVERIFY(setAside.isEmpty());
#endif
    }

    // the whole update of a Windows-style folder against two published launchers: offered, downloaded, checked,
    // replaced, the relaunch recorded; the next start removes the set-aside files and says so once
    void updateAFolder() {
        const QString t = ReleaseCatalogue::hostTarget();
        const QString pattern = manifest_.launcherAssetPattern(t);
        FixtureReleaseServer server;
        QVERIFY(server.start());
        AppData data("TwinsanityEvolved");
        data.setValue("releases_url", server.releasesUrl());
        QVERIFY(data.save());
        QTemporaryDir src;
        QVERIFY(writeFile(src.path() + "/TwinsanityEvolved/TwinsanityEvolved.exe", "launcher v0.2.1"));
        QVERIFY(writeFile(src.path() + "/TwinsanityEvolved/Qt6Core.dll", "core v0.2.1"));
        QVERIFY(writeFile(src.path() + "/TwinsanityEvolved/BUILD-ID", "TwinsanityEvolved launcher v0.2.1 (windows-x64)\nbuilt   2026-10-26T10:00:00Z\n"));
        const QByteArray zip = zipTree(src.path() + "/TwinsanityEvolved");
        server.addRelease("v0.2.0", {{ReleaseCatalogue::assetName(pattern, "v0.2.0", t), "the old one"}}, QDateTime::currentDateTimeUtc().addDays(-9));
        server.addRelease("v0.2.1", {{ReleaseCatalogue::assetName(pattern, "v0.2.1", t), zip}}, QDateTime::currentDateTimeUtc().addDays(-1));
        const QString folder = home_.path() + "/Launcher Folder";
        QVERIFY(writeFile(folder + "/TwinsanityEvolved.exe", "launcher v0.2.0"));
        QVERIFY(writeFile(folder + "/Qt6Core.dll", "core v0.2.0"));
        QVERIFY(writeFile(folder + "/BUILD-ID", "TwinsanityEvolved launcher v0.2.0 (windows-x64)\n"));
        SelfUpdate su(&data, &manifest_);
        su.setRunning(runningAt("v0.2.0", "folder", folder, "TwinsanityEvolved.exe"));
        QStringList relaunched;
        su.setRelaunch([&](const RunningLauncher& next, QString*) { relaunched << next.path; return true; });
        QSignalSpy quit(&su, &SelfUpdate::quitRequested);
        su.check();
        QVERIFY(QTest::qWaitFor([&] { return su.state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(su.state(), SelfUpdate::Available);
        QCOMPARE(su.release().tag, QStringLiteral("v0.2.1"));
        QCOMPARE(su.statusText(), QStringLiteral("A new launcher is ready: v0.2.1."));
        QCOMPARE(su.cannotReplace(), SelfUpdate::NoProblem);
        su.start();
        QVERIFY(QTest::qWaitFor([&] { return su.state() == SelfUpdate::Restarting || su.state() == SelfUpdate::Failed; }, 20000));
        QVERIFY2(su.state() == SelfUpdate::Restarting, qPrintable(su.problemText()));
        QCOMPARE(quit.count(), 1);
        QCOMPARE(relaunched, QStringList{folder});
        QCOMPARE(readFile(folder + "/TwinsanityEvolved.exe"), QByteArray("launcher v0.2.1"));
        QCOMPARE(readFile(folder + "/Qt6Core.dll"), QByteArray("core v0.2.1"));
        QCOMPARE(readFile(folder + "/TwinsanityEvolved.exe.replaced"), QByteArray("launcher v0.2.0"));
        QVERIFY(!QFileInfo::exists(folder + "/.update-v0.2.1"));
        QVERIFY(!QFileInfo::exists(data.downloadsDir() + "/" + ReleaseCatalogue::assetName(pattern, "v0.2.1", t)));   // the download is not kept
        QVERIFY(QFileInfo(su.logPath()).isFile());
        QVERIFY(readFile(su.logPath()).contains("matches SHA256SUMS"));
        // the next start: the new launcher (v0.2.1) removes what was set aside, once, and says so
        AppData next("TwinsanityEvolved");
        QCOMPARE(next.value("launcher_update").toObject().value("aside").toArray().size(), 3);   // the program, its library, BUILD-ID
        SelfUpdate after(&next, &manifest_);
        after.setRunning(runningAt("v0.2.1", "folder", folder, "TwinsanityEvolved.exe"));
        after.finishAfterRelaunch();
        QCOMPARE(after.finishedText(), QStringLiteral("The launcher was updated to v0.2.1."));
        QVERIFY(after.updatedOk());
        QVERIFY(!QFileInfo::exists(folder + "/TwinsanityEvolved.exe.replaced"));
        QVERIFY(!QFileInfo::exists(folder + "/Qt6Core.dll.replaced"));
        QVERIFY(!QFileInfo::exists(folder + "/BUILD-ID.replaced"));
        QVERIFY(QFileInfo::exists(folder + "/TwinsanityEvolved.exe"));
        QVERIFY(!AppData("TwinsanityEvolved").state().contains("launcher_update"));
        // and nothing newer now
        after.check();
        QVERIFY(QTest::qWaitFor([&] { return after.state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(after.state(), SelfUpdate::UpToDate);
        server.stop();
    }

    // an AppImage file: the new one beside it, swapped in; the old one set aside and removed by the next start
    void updateAnAppImage() {
        const QString t = ReleaseCatalogue::hostTarget();
        const QString pattern = manifest_.launcherAssetPattern(t);
        FixtureReleaseServer server;
        QVERIFY(server.start());
        AppData data("TwinsanityEvolved");
        data.setValue("releases_url", server.releasesUrl());
        data.remove("launcher_update");
        QVERIFY(data.save());
        const QByteArray image = QByteArray("\x7f" "ELF", 4) + QByteArray(2048, 'n');
        server.addRelease("v0.2.1", {{ReleaseCatalogue::assetName(pattern, "v0.2.1", t), image}}, QDateTime::currentDateTimeUtc().addDays(-1));
        const QString file = home_.path() + "/Apps/TwinsanityEvolved-launcher.AppImage";
        QVERIFY(writeFile(file, QByteArray("\x7f" "ELF", 4) + "old"));
        SelfUpdate su(&data, &manifest_);
        su.setRunning(runningAt("v0.2.0", "appimage", file, "TwinsanityEvolved"));
        su.setRelaunch([](const RunningLauncher&, QString*) { return true; });
        su.setStartWhenFound(true);   // --update-launcher
        su.check();
        QVERIFY(QTest::qWaitFor([&] { return su.state() == SelfUpdate::Restarting || su.state() == SelfUpdate::Failed || su.state() == SelfUpdate::UpToDate; }, 20000));
        QVERIFY2(su.state() == SelfUpdate::Restarting, qPrintable(su.problemText()));
        QCOMPARE(readFile(file), image);
#ifndef Q_OS_WIN   // (Windows has no executable bit: a file is executable by its extension)
        QVERIFY(QFileInfo(file).isExecutable());
#endif
        const QString aside = home_.path() + "/Apps/.TwinsanityEvolved-launcher-replaced-v0.2.0.AppImage";
        QCOMPARE(readFile(aside), QByteArray("\x7f" "ELF", 4) + "old");
        SelfUpdate after(&data, &manifest_);
        after.setRunning(runningAt("v0.2.1", "appimage", file, "TwinsanityEvolved"));
        after.finishAfterRelaunch();
        QVERIFY(!QFileInfo::exists(aside));
        QCOMPARE(after.finishedText(), QStringLiteral("The launcher was updated to v0.2.1."));
        server.stop();
    }

#ifdef Q_OS_MACOS
    // a macOS bundle: a real program (a copy of /usr/bin/true), ad-hoc signed, zipped with its top folder; unpacked
    // beside the running bundle, checked by codesign, swapped in; a zip of another program and a damaged download are
    // refused with the running bundle untouched
    void updateABundle() {
        const QString t = ReleaseCatalogue::hostTarget();
        const QString pattern = manifest_.launcherAssetPattern(t);
        const QString id = QStringLiteral("dev.nativeee.launcher.sles-52568");
        QTemporaryDir src;
        auto bundle = [&](const QString& dir, const QString& version, const QString& bundleId) {
            QVERIFY(writeFile(dir + "/Contents/Info.plist", infoPlist(bundleId, "ps2r_launcher", version)));
            QVERIFY(QDir().mkpath(dir + "/Contents/MacOS"));
            QFile::remove(dir + "/Contents/MacOS/ps2r_launcher");
            QVERIFY(QFile::copy("/usr/bin/true", dir + "/Contents/MacOS/ps2r_launcher"));
            QVERIFY(QFile::setPermissions(dir + "/Contents/MacOS/ps2r_launcher", QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner |
                                                                                    QFileDevice::ReadOther | QFileDevice::ExeOther));
            QVERIFY(writeFile(dir + "/Contents/Resources/BUILD-ID", ("TwinsanityEvolved launcher " + version + "\nbuilt   2026-10-26T10:00:00Z\n").toUtf8()));
            QVERIFY(writeFile(dir + "/Contents/Resources/version.txt", version.toUtf8()));
            QProcess cs;
            cs.start("/usr/bin/codesign", {"--force", "--sign", "-", dir});
            QVERIFY(cs.waitForFinished(60000));
            QVERIFY2(cs.exitCode() == 0, cs.readAllStandardError().constData());
        };
        bundle(src.path() + "/new/TwinsanityEvolved.app", "v0.2.1", id);
        bundle(src.path() + "/other/TwinsanityEvolved.app", "v0.2.1", "com.example.other");
        // zipped the way tools/ci/release_launcher.sh zips the release's bundle (ditto keeps the modes and the links; Qt's
        // zip writer drops the owner's execute bit -- it is the reader the launcher uses, never the writer)
        auto ditto = [&](const QString& app) {
            const QString out = src.path() + "/" + QFileInfo(QFileInfo(app).absolutePath()).fileName() + ".zip";
            QProcess p;
            p.start("/usr/bin/ditto", {"-c", "-k", "--norsrc", "--noextattr", "--noacl", "--noqtn", "--keepParent", app, out});
            if (!p.waitForFinished(60000) || p.exitCode() != 0) return QByteArray();
            return readFile(out);
        };
        const QByteArray zip = ditto(src.path() + "/new/TwinsanityEvolved.app");
        const QByteArray otherZip = ditto(src.path() + "/other/TwinsanityEvolved.app");
        QVERIFY(!zip.isEmpty() && !otherZip.isEmpty());
        const QString apps = home_.path() + "/Applications";
        const QString app = apps + "/TwinsanityEvolved.app";
        bundle(app, "v0.2.0", id);
        FixtureReleaseServer server;
        QVERIFY(server.start());
        AppData data("TwinsanityEvolved");
        data.setValue("releases_url", server.releasesUrl());
        data.remove("launcher_update");
        QVERIFY(data.save());
        server.addRelease("v0.2.0", {{ReleaseCatalogue::assetName(pattern, "v0.2.0", t), "old"}}, QDateTime::currentDateTimeUtc().addDays(-9));
        server.addRelease("v0.2.1", {{ReleaseCatalogue::assetName(pattern, "v0.2.1", t), otherZip}}, QDateTime::currentDateTimeUtc().addDays(-1));
        SelfUpdate su(&data, &manifest_);
        RunningLauncher run = runningAt("v0.2.0", "app", app, "ps2r_launcher");
        su.setRunning(run);
        int relaunches = 0;
        su.setRelaunch([&](const RunningLauncher&, QString*) { ++relaunches; return true; });
        auto runUpdate = [&] {
            su.check();
            QVERIFY(QTest::qWaitFor([&] { return su.state() != SelfUpdate::Checking; }, 20000));
            QCOMPARE(su.state(), SelfUpdate::Available);
            su.start();
            QVERIFY(QTest::qWaitFor([&] { return su.state() == SelfUpdate::Restarting || su.state() == SelfUpdate::Failed; }, 120000));
        };
        // another program's bundle in the zip: refused, nothing changed
        runUpdate();
        QCOMPARE(su.state(), SelfUpdate::Failed);
        QCOMPARE(su.problem(), SelfUpdate::Broken);
        QCOMPARE(su.problemText(), QStringLiteral("The download is not this launcher. Nothing was changed."));
        QCOMPARE(readFile(app + "/Contents/Resources/version.txt"), QByteArray("v0.2.0"));
        QCOMPARE(QDir(apps).entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden), QStringList{"TwinsanityEvolved.app"});
        // a damaged download: refused, deleted, nothing changed
        server.clear();
        server.addRelease("v0.2.0", {{ReleaseCatalogue::assetName(pattern, "v0.2.0", t), "old"}}, QDateTime::currentDateTimeUtc().addDays(-9));
        server.addRelease("v0.2.1", {{ReleaseCatalogue::assetName(pattern, "v0.2.1", t), zip}}, QDateTime::currentDateTimeUtc().addDays(-1));
        server.corrupt(ReleaseCatalogue::assetName(pattern, "v0.2.1", t));
        runUpdate();
        QCOMPARE(su.problem(), SelfUpdate::Damaged);
        QCOMPARE(readFile(app + "/Contents/Resources/version.txt"), QByteArray("v0.2.0"));
        // the real one
        server.clear();
        server.addRelease("v0.2.0", {{ReleaseCatalogue::assetName(pattern, "v0.2.0", t), "old"}}, QDateTime::currentDateTimeUtc().addDays(-9));
        server.addRelease("v0.2.1", {{ReleaseCatalogue::assetName(pattern, "v0.2.1", t), zip}}, QDateTime::currentDateTimeUtc().addDays(-1));
        runUpdate();
        QVERIFY2(su.state() == SelfUpdate::Restarting, qPrintable(su.problemText() + "\n" + QString::fromUtf8(readFile(su.logPath()))));
        QCOMPARE(relaunches, 1);
        QCOMPARE(readFile(app + "/Contents/Resources/version.txt"), QByteArray("v0.2.1"));
        const QString aside = apps + "/.TwinsanityEvolved-replaced-v0.2.0.app";
        QCOMPARE(readFile(aside + "/Contents/Resources/version.txt"), QByteArray("v0.2.0"));
        QProcess verify;
        verify.start("/usr/bin/codesign", {"--verify", "--deep", "--strict", app});
        QVERIFY(verify.waitForFinished(60000));
        QVERIFY2(verify.exitCode() == 0, verify.readAllStandardError().constData());
        QVERIFY(readFile(su.logPath()).contains("codesign --verify --deep --strict: exit 0"));
        // the next start removes the copy set aside (and only it)
        QVERIFY(writeFile(apps + "/.Unrelated-replaced-x.app/Contents/x", "someone else's"));
        QJsonObject rec = data.value("launcher_update").toObject();
        QJsonArray aside2 = rec.value("aside").toArray();
        aside2 << apps + "/.Unrelated-replaced-x.app" << home_.path() + "/Documents";   // hand-edited: never removed when outside the rule
        rec["aside"] = aside2;
        data.setValue("launcher_update", rec);
        QVERIFY(data.save());
        QVERIFY(QDir().mkpath(home_.path() + "/Documents"));
        AppData next("TwinsanityEvolved");
        SelfUpdate after(&next, &manifest_);
        run.version = "v0.2.1";
        after.setRunning(run);
        after.finishAfterRelaunch();
        QVERIFY(!QFileInfo::exists(aside));
        QVERIFY(QFileInfo::exists(home_.path() + "/Documents"));   // not beside the bundle: left alone
        QCOMPARE(after.finishedText(), QStringLiteral("The launcher was updated to v0.2.1."));
        server.stop();
    }
#endif

    // the refusals: where it cannot replace itself the offer says why and gives the release's page
    void refusals() {
        // this test program is not a launcher installed from a release (no BUILD-ID beside it): it never replaces itself
        const RunningLauncher me = SelfUpdate::detect();
        QVERIFY(me.kind.isEmpty());
        QVERIFY(me.made.isValid());
        QVERIFY(!me.version.isEmpty());
        AppData data("TwinsanityEvolved");
        SelfUpdate su(&data, &manifest_);
        QString text;
        su.setRunning(runningAt("v0.2.0", QString(), QString(), "x"));
        QCOMPARE(su.cannotReplace(&text), SelfUpdate::NotInstalled);
        QCOMPARE(text, QStringLiteral("This copy of the launcher cannot replace itself. Download the new one from its page."));
        QCOMPARE(su.downloadPage(), QUrl("https://example.invalid/project/releases/latest"));
        const QString folder = home_.path() + "/Locked";
        QVERIFY(writeFile(folder + "/TwinsanityEvolved.exe", "x"));
        su.setRunning(runningAt("v0.2.0", "folder", folder, "TwinsanityEvolved.exe"));
        QCOMPARE(su.cannotReplace(), SelfUpdate::NoProblem);
        bool game = true;
        su.setGameRunning([&] { return game; });
        QCOMPARE(su.cannotReplace(&text), SelfUpdate::GameRunning);
        QCOMPARE(text, QStringLiteral("Close the game first: the launcher opens again to update."));
        game = false;
#ifndef Q_OS_WIN
        QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::ExeOwner));
        QCOMPARE(su.cannotReplace(&text), SelfUpdate::NotWritable);
        QVERIFY2(text.startsWith("The launcher is in ~/Locked, which this account cannot change."), qPrintable(text));
        QVERIFY(QFile::setPermissions(folder, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner));
#endif
        su.setRunning(runningAt("v0.2.0", "app", "/private/var/folders/xy/T/AppTranslocation/0000/d/TwinsanityEvolved.app", "ps2r_launcher"));
        QCOMPARE(su.cannotReplace(&text), SelfUpdate::Translocated);
        QVERIFY(text.contains("Move TwinsanityEvolved to your Applications folder"));
        // offline at start: nothing is offered and nothing is said
        data.setValue("releases_url", "http://127.0.0.1:9/releases");
        su.setRunning(runningAt("v0.2.0", "folder", folder, "TwinsanityEvolved.exe"));
        su.check();
        QVERIFY(QTest::qWaitFor([&] { return su.state() != SelfUpdate::Checking; }, 40000));
        QCOMPARE(su.state(), SelfUpdate::Idle);
        QVERIFY(su.statusText().isEmpty());
    }
    // A file:// list, as the hand-test kit serves its releases (no web server): the list, SHA256SUMS and the launcher's
    // file are read from a folder; the running launcher is not in the list, so the release's date decides
    void updateFromAFileList() {
        const QString t = ReleaseCatalogue::hostTarget();
        const QString pattern = manifest_.launcherAssetPattern(t);
        QTemporaryDir rel, src;
        QVERIFY(writeFile(src.path() + "/TwinsanityEvolved/TwinsanityEvolved.exe", "launcher v0.1.9-handtest"));
        QVERIFY(writeFile(src.path() + "/TwinsanityEvolved/BUILD-ID", "TwinsanityEvolved launcher v0.1.9-handtest (windows-x64)\n"));
        const QByteArray zip = zipTree(src.path() + "/TwinsanityEvolved");
        const QString name = ReleaseCatalogue::assetName(pattern, QStringLiteral("v0.1.9-handtest"), t);
        QVERIFY(writeFile(rel.path() + "/launcher-update/" + name, zip));
        const QByteArray sums = QCryptographicHash::hash(zip, QCryptographicHash::Sha256).toHex() + "  " + name.toUtf8() + "\n";
        QVERIFY(writeFile(rel.path() + "/launcher-update/SHA256SUMS", sums));
        auto asset = [&](const QString& file, qint64 bytes) {
            const QString url = QUrl::fromLocalFile(rel.path() + "/launcher-update/" + file).toString();
            return QJsonObject{{"name", file}, {"size", double(bytes)}, {"browser_download_url", url}, {"url", url}};
        };
        const QJsonObject launcherRelease{{"tag_name", "v0.1.9-handtest"}, {"draft", false}, {"prerelease", false},
                                          {"published_at", QDateTime::currentDateTimeUtc().addDays(-1).toString(Qt::ISODate)},
                                          {"html_url", QUrl::fromLocalFile(rel.path() + "/launcher-update/").toString()},
                                          {"assets", QJsonArray{asset(name, zip.size()), asset("SHA256SUMS", sums.size())}}};
        const QJsonObject gameRelease{{"tag_name", "v0.1.8-152-gbe96e50"}, {"draft", false}, {"prerelease", false},
                                      {"published_at", QDateTime::currentDateTimeUtc().addDays(-2).toString(Qt::ISODate)},
                                      {"assets", QJsonArray{QJsonObject{{"name", "twinsanityevolved-v0.1.8-152-gbe96e50-" + t + ".zip"}, {"size", 1.0},
                                                                        {"browser_download_url", QUrl::fromLocalFile(rel.path() + "/game.zip").toString()}}}}};
        QVERIFY(writeFile(rel.path() + "/releases.json", QJsonDocument(QJsonArray{launcherRelease, gameRelease}).toJson()));
        AppData data("TwinsanityEvolved");
        data.remove("launcher_update");
        data.setValue("releases_url", QUrl::fromLocalFile(rel.path() + "/releases.json").toString());
        QVERIFY(data.save());
        const QString folder = home_.path() + "/Kit Launcher";
        QVERIFY(writeFile(folder + "/TwinsanityEvolved.exe", "launcher v0.1.8-166-g6cb54cd"));
        QVERIFY(writeFile(folder + "/BUILD-ID", "TwinsanityEvolved launcher v0.1.8-166-g6cb54cd (windows-x64)\n"));
        SelfUpdate su(&data, &manifest_);
        su.setRunning(runningAt("v0.1.8-166-g6cb54cd", "folder", folder, "TwinsanityEvolved.exe"));
        bool relaunched = false;
        su.setRelaunch([&](const RunningLauncher&, QString*) { relaunched = true; return true; });
        su.check();
        QVERIFY(QTest::qWaitFor([&] { return su.state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(int(su.state()), int(SelfUpdate::Available));
        QCOMPARE(su.release().tag, QStringLiteral("v0.1.9-handtest"));
        QVERIFY(readFile(su.logPath()).contains("is not in the list; v0.1.9-handtest was published"));
        su.start();
        QVERIFY(QTest::qWaitFor([&] { return su.state() == SelfUpdate::Restarting || su.state() == SelfUpdate::Failed; }, 20000));
        QVERIFY2(su.state() == SelfUpdate::Restarting, qPrintable(su.problemText()));
        QVERIFY(relaunched);
        QCOMPARE(readFile(folder + "/TwinsanityEvolved.exe"), QByteArray("launcher v0.1.9-handtest"));
        // the new one: its own tag is in the list now, so nothing is newer
        AppData next("TwinsanityEvolved");
        SelfUpdate after(&next, &manifest_);
        after.setRunning(runningAt("v0.1.9-handtest", "folder", folder, "TwinsanityEvolved.exe"));
        after.finishAfterRelaunch();
        QCOMPARE(after.finishedText(), QStringLiteral("The launcher was updated to v0.1.9-handtest."));
        after.check();
        QVERIFY(QTest::qWaitFor([&] { return after.state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(int(after.state()), int(SelfUpdate::UpToDate));
        // a size in the list that is not the file's: the download is refused, nothing is replaced
        QVERIFY(writeFile(folder + "/TwinsanityEvolved.exe", "launcher v0.1.8-166-g6cb54cd"));
        QJsonObject wrong = launcherRelease;
        wrong["assets"] = QJsonArray{asset(name, zip.size() + 1), asset("SHA256SUMS", sums.size())};
        QVERIFY(writeFile(rel.path() + "/releases.json", QJsonDocument(QJsonArray{wrong, gameRelease}).toJson()));
        SelfUpdate again(&next, &manifest_);
        again.setRunning(runningAt("v0.1.8-166-g6cb54cd", "folder", folder, "TwinsanityEvolved.exe"));
        again.setRelaunch([&](const RunningLauncher&, QString*) { return true; });
        again.check();
        QVERIFY(QTest::qWaitFor([&] { return again.state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(int(again.state()), int(SelfUpdate::Available));
        again.start();
        QVERIFY(QTest::qWaitFor([&] { return again.state() == SelfUpdate::Failed || again.state() == SelfUpdate::Restarting; }, 20000));
        QCOMPARE(int(again.state()), int(SelfUpdate::Failed));
        QCOMPARE(readFile(folder + "/TwinsanityEvolved.exe"), QByteArray("launcher v0.1.8-166-g6cb54cd"));
        next.remove("launcher_update");
        next.remove("releases_url");
        QVERIFY(next.save());
    }
    // --wait-for (the Windows / Linux relaunch): the new launcher waits for the old process, started detached as the old
    // launcher starts the new one (so it is nobody's lingering child here either)
    void waitsForTheOldLauncher() {
#ifdef Q_OS_WIN
        QSKIP("POSIX processes here (Windows: the process handle, built and run by the Windows job)");
#else
        qint64 pid = 0;
        QVERIFY(QProcess::startDetached(QStringLiteral("/bin/sleep"), {QStringLiteral("1")}, QString(), &pid));
        QElapsedTimer t;
        t.start();
        QVERIFY(SelfUpdate::waitForExit(pid, 10000));
        QVERIFY2(t.elapsed() >= 500, qPrintable(QString::number(t.elapsed())));
        t.restart();
        QVERIFY(SelfUpdate::waitForExit(pid, 100));   // gone: at once
        QVERIFY(t.elapsed() < 100);
        qint64 stays = 0;
        QVERIFY(QProcess::startDetached(QStringLiteral("/bin/sleep"), {QStringLiteral("30")}, QString(), &stays));
        t.restart();
        QVERIFY(!SelfUpdate::waitForExit(stays, 300));   // still running: the time limit decides
        QVERIFY(t.elapsed() >= 300 && t.elapsed() < 3000);
        QVERIFY(::kill(pid_t(stays), SIGKILL) == 0);
        QVERIFY(SelfUpdate::waitForExit(stays, 10000));
        QVERIFY(SelfUpdate::waitForExit(QCoreApplication::applicationPid(), 100));   // never this process itself
#endif
    }
};

QTEST_MAIN(TestLauncherSelfUpdate)
#include "test_launcher_self_update.moc"
