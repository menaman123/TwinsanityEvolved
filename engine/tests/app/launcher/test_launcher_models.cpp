// tests/app/launcher/test_launcher_models.cpp -- the launcher's models without a window: the manifest (schema 2), the
// release choice and SHA256SUMS, the game folder (ps2r.ini defaults, relative folders, disc-image.txt), the install
// end to end against a local release server (placed, the user's files kept, a damaged download refused, offline, no
// release for the target), the options-file round trip, the card texts.
#include <QtTest/QtTest>
#include <QUrl>
#include <QSignalSpy>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#if __has_include(<QtCore/private/qzipwriter_p.h>)
#include <QtCore/private/qzipwriter_p.h>
#else
#include <QtGui/private/qzipwriter_p.h>
#endif
#include "app_data.h"
#include "app/host/disc_identity.h"
#include "app/host/disc_regions.h"
#include "disc_scanner.h"
#include "disc_text.h"
#include "fixture_release_server.h"
#include "game_folder.h"
#include "game_library.h"
#include "generated_artwork.h"
#include "ini_settings.h"
#include "launcher_app.h"
#include "release_catalogue.h"
#include "release_installer.h"
#include "title_manifest.h"
#include "vita_folder.h"
using namespace ps2r::launcher;

namespace {
// a tiny ISO 9660 volume (PVD 16, terminator 17, the root directory 18, files from 20): byte patterns, no game data
void putBoth32(QByteArray& b, int at, quint32 v) {
    for (int i = 0; i < 4; ++i) { b[at + i] = char(v >> (8 * i)); b[at + 7 - i] = char(v >> (8 * i)); }
}
QByteArray isoRecord(quint32 lba, quint32 size, bool dir, const QByteArray& id) {
    QByteArray r(33 + id.size() + ((33 + id.size()) & 1), '\0');
    r[0] = char(r.size());
    putBoth32(r, 2, lba);
    putBoth32(r, 10, size);
    r[25] = dir ? 2 : 0;
    r[28] = 1; r[31] = 1;
    r[32] = char(id.size());
    r.replace(33, id.size(), id);
    return r;
}
QByteArray tinyIso(const QVector<QPair<QString, QByteArray>>& files) {
    quint32 next = 20;
    QVector<quint32> lbas;
    for (const auto& f : files) { lbas << next; next += quint32(qMax<qsizetype>(1, (f.second.size() + 2047) / 2048)); }
    QByteArray img(int(next) * 2048, '\0');
    QByteArray root = isoRecord(18, 2048, true, QByteArray(1, '\0')) + isoRecord(18, 2048, true, QByteArray(1, '\1'));
    for (int i = 0; i < files.size(); ++i) {
        root += isoRecord(lbas[i], quint32(files[i].second.size()), false, (files[i].first + ";1").toLatin1());
        img.replace(int(lbas[i]) * 2048, files[i].second.size(), files[i].second);
    }
    img.replace(18 * 2048, root.size(), root);
    QByteArray pvd(2048, '\0');
    pvd[0] = 1;
    pvd.replace(1, 5, "CD001");
    pvd[6] = 1;
    pvd.replace(40, 32, QByteArray("CACHETEST").leftJustified(32, ' '));
    putBoth32(pvd, 80, next);
    pvd[128] = 0x00; pvd[129] = 0x08; pvd[130] = 0x08; pvd[131] = 0x00;
    const QByteArray rootRec = isoRecord(18, 2048, true, QByteArray(1, '\0'));
    pvd.replace(156, rootRec.size(), rootRec);
    img.replace(16 * 2048, 2048, pvd);
    QByteArray term(2048, '\0');
    term[0] = static_cast<char>(-1);   // 0xFF: the set terminator's type
    term.replace(1, 5, "CD001");
    img.replace(17 * 2048, 2048, term);
    return img;
}

const char* kManifest = R"JSON({
  "schema": 2, "id": "SLES-52568", "name": "Crash Twinsanity", "product": "TwinsanityEvolved", "app_data_folder": "TwinsanityEvolved",
  "binary_name": "twinsanityevolved",
  "disc": { "manifests": ["discs/pal.json"], "plays": ["SLES_525.68"], "plays_note": "Plays the PAL disc.", "extensions": [".iso", ".bin"] },
  "release": { "releases_url": "https:\/\/example.invalid/releases", "zip_asset": "twinsanityevolved-{tag}-{target}.zip",
               "vita_asset": "twinsanityevolved-{tag}-vita.vpk", "sums_asset": "SHA256SUMS", "install_bytes": 1000 },
  "targets": { "computers": [ { "id": "macos-arm64", "label": "macOS (Apple silicon)", "exe": "twinsanityevolved" },
                              { "id": "windows-x64", "label": "Windows", "exe": "twinsanityevolved.exe" } ],
               "vita": { "label": "PS Vita", "file": "twinsanityevolved.vpk", "note": ["one", "two", "three"] } },
  "run": { "iso_flag": "--iso", "config_flag": "--config", "args": ["--trace-state"], "title_state_line": "game state @ boundary", "title_state_name": "Title (7)" },
  "game_folder": { "default": "~/Games/Crash Twinsanity", "settings": "ps2r.ini", "memcards": "memcards", "mods": "mods", "disc_file": "disc-image.txt",
                   "protected": ["ps2r.ini", "memcards", "memcards-mods", "mods", "userdata", "disc-image.txt"], "settings_header": "# test header" },
  "settings": { "groups": [ { "title": "Picture", "rows": [
    { "key": "video.frame_rate", "label": "Frame rate", "kind": "choice", "default": "60", "write_default": true, "choices": [ {"value": "30"}, {"value": "60"}, {"value": "uncapped", "label": "Uncapped"} ] },
    { "key": "video.vsync", "label": "Vertical sync", "kind": "switch", "default": "off", "write_default": true },
    { "key": "display.tick_lock", "label": "Frame rate on PS Vita", "kind": "choice", "default": "auto", "choices": [ {"value": "auto"}, {"value": "60"} ] },
    { "key": "paths.memcards", "label": "Memory cards", "kind": "folder", "default": "memcards", "write_default": true },
    { "key": "disc-image.txt", "label": "Disc image", "kind": "disc" } ] } ] },
  "mods": { "builtin": [ { "id": "level_select", "name": "Level select", "help": "x" } ] },
  "scan": { "folders": ["~/Downloads", "${launcher}"], "removable": false, "max_depth": 2, "min_bytes": 1024 }
})JSON";

QByteArray makeZip(const QString& root, const QHash<QString, QByteArray>& files, const QString& exe) {
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QZipWriter zip(&buf);
    zip.addDirectory(root);
    for (auto it = files.begin(); it != files.end(); ++it) {
        zip.setCreationPermissions(it.key() == exe ? QFile::Permissions(0x7755) : QFile::Permissions(0x6644));
        zip.addFile(root + "/" + it.key(), it.value());
    }
    zip.close();
    return buf.data();
}

bool writeFile(const QString& path, const QByteArray& bytes) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size();
}
QByteArray readFile(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}
// a mod folder: mod.json (schema 2, the kind) and its files (no game data: byte patterns)
bool writeMod(const QString& dir, const QString& id, const QString& kind, const QVector<QPair<QString, QByteArray>>& files) {
    QJsonObject m{{"schema", 2}, {"id", id}, {"kind", kind}, {"name", id + " test"}};
    if (kind == QLatin1String("language")) m["language"] = QJsonObject{{"id", id}, {"label", id + " (text)"}, {"text_bank", "data/" + id + ".bank"}};
    bool ok = writeFile(dir + "/mod.json", QJsonDocument(m).toJson());
    for (const auto& f : files) ok = ok && writeFile(dir + "/" + f.first, f.second);
    return ok;
}

TitleManifest loadedManifest(const QString& baseDir) {
    TitleManifest m;
    QString err;
    m.loadJson(QJsonDocument::fromJson(kManifest).object(), &err, baseDir);
    return m;
}
}  // namespace

class TestLauncherModels : public QObject {
    Q_OBJECT
    QTemporaryDir home_;

private slots:
    void initTestCase() {
        qputenv("HOME", home_.path().toUtf8());
        qputenv("USERPROFILE", home_.path().toUtf8());
        qputenv("LOCALAPPDATA", home_.path().toUtf8());
        qunsetenv("GITHUB_TOKEN");
        qunsetenv("GH_TOKEN");
    }

    // the PS Vita folder: the console's data folder made from a game folder -- its settings without the computer's
    // own keys, a copy of the language packs and of the mods switched on, the disc's name, the README; made again: what is
    // there stays, a mod switched off leaves the copy; a disc name a memory card does not hold as it is: game.iso
    void vitaFolder() {
        QJsonObject root = QJsonDocument::fromJson(kManifest).object();
        QJsonObject targets = root["targets"].toObject();
        QJsonObject vita = targets["vita"].toObject();
        vita["note"] = QJsonArray{"Copy {vpk} with VitaShell.", "Copy {data_dir} so that it is {data}.", "Copy your disc to {data}/{disc}."};
        vita["folder"] = QJsonObject{{"name", "vita"}, {"data_dir", "ux0/data/twinsanity"}, {"readme", "README-vita.txt"}, {"default_disc", "game.iso"},
                                     {"settings_left_out", QJsonArray{"paths.memcards", "paths.mods", "game.hz", "video.scale"}}};
        targets["vita"] = vita;
        root["targets"] = targets;
        TitleManifest m;
        QString err;
        QVERIFY2(m.loadJson(root, &err, home_.path()), qPrintable(err));
        QCOMPARE(m.vitaDataDir(), QStringLiteral("ux0/data/twinsanity"));
        QCOMPARE(m.vitaSettingsLeftOut().size(), 4);
        const QString game = home_.path() + "/Games/Vita Folder";
        IniSettings ini(game + "/ps2r.ini");
        QVERIFY(ini.set("game.text_language", "arabic"));
        QVERIFY(ini.set("game.hz", "60"));
        QVERIFY(ini.set("video.scale", "3"));
        QVERIFY(ini.set("paths.memcards", "memcards"));
        QVERIFY(ini.set("paths.mods", "mods"));
        QVERIFY(ini.set("display.tick_lock", "30"));
        QVERIFY(ini.set("mods.musicpack.enabled", "on"));
        QVERIFY(ini.set("mods.other.enabled", "off"));
        QVERIFY(ini.set("mods.order", "musicpack"));
        QVERIFY(writeMod(game + "/mods/arabic", "arabic", "language", {{"data/arabic.bank", "bank bytes"}}));
        QVERIFY(writeMod(game + "/mods/musicpack", "musicpack", "disc", {{"disc/CRASH6/MUSIC.MB", QByteArray(300000, 'k')}, {"data/a.bin", "aaaa"}}));
        QVERIFY(writeMod(game + "/mods/other", "other", "disc", {{"data/o.bin", "oooo"}}));
        QVERIFY(writeMod(game + "/mods/Broken Name", "Broken Name", "disc", {{"data/b.bin", "bbbb"}}));   // the game does not read it
        QVERIFY(writeFile(game + "/mods/.import-tmp/half.bin", "half"));                                     // hidden: never copied
        const QString disc = home_.path() + "/Discs/Crash Twinsanity (Europe).iso";
        QVERIFY(writeFile(disc, "not a disc"));
        QVERIFY(writeFile(game + "/disc-image.txt", (disc + "\n").toUtf8()));
        const QString folder = VitaFolder::folderIn(&m, game);
        QCOMPARE(folder, game + "/vita");
        QCOMPARE(VitaFolder::homeOf(&m, folder), game);
        QCOMPARE(VitaFolder::homeOf(&m, game), QString());
        VitaFolderReport r;
        QVERIFY2(VitaFolder::make(&m, game, folder, QString(), &r), qPrintable(r.error));
        const QString data = folder + "/ux0/data/twinsanity";
        QCOMPARE(r.dataDir, data);
        // the settings: the computer's keys left out, the rest as the game folder has them
        IniSettings copy(data + "/ps2r.ini");
        QCOMPARE(copy.get("game.text_language"), QStringLiteral("arabic"));
        QCOMPARE(copy.get("display.tick_lock"), QStringLiteral("30"));
        QCOMPARE(copy.get("mods.musicpack.enabled"), QStringLiteral("on"));
        QCOMPARE(copy.get("mods.order"), QStringLiteral("musicpack"));
        for (const char* gone : {"game.hz", "video.scale", "paths.memcards", "paths.mods"}) QVERIFY2(!copy.has(gone), gone);
        // the mods: the one switched on and the language pack, in the overlay order; not the one off, not the unreadable one
        QCOMPARE(r.mods, (QStringList{"musicpack", "arabic"}));
        QCOMPARE(QFileInfo(data + "/mods/musicpack/disc/CRASH6/MUSIC.MB").size(), qint64(300000));
        QCOMPARE(readFile(data + "/mods/musicpack/data/a.bin"), QByteArray("aaaa"));
        QCOMPARE(readFile(data + "/mods/arabic/data/arabic.bank"), QByteArray("bank bytes"));
        QVERIFY(!QFileInfo::exists(data + "/mods/other"));
        QVERIFY(!QFileInfo::exists(data + "/mods/Broken Name"));
        QVERIFY(!QFileInfo::exists(data + "/mods/.import-tmp"));
        QVERIFY(r.modBytes >= 300000 + 4 + 10);
        QCOMPARE(r.copied, 5);   // musicpack: mod.json + 2 files; arabic: mod.json + 1 file
        QCOMPARE(r.kept, 0);
        // the disc: its file name (the player copies the image itself)
        QCOMPARE(r.disc, QStringLiteral("Crash Twinsanity (Europe).iso"));
        QCOMPARE(readFile(data + "/disc-image.txt"), QByteArray("Crash Twinsanity (Europe).iso\n"));
        QVERIFY(!QFileInfo::exists(data + "/Crash Twinsanity (Europe).iso"));
        // the README: the steps with the names filled in, the mods' size, the saves not copied
        const QString readme = QString::fromUtf8(readFile(folder + "/README-vita.txt"));
        QVERIFY2(readme.contains("1. Copy twinsanityevolved.vpk with VitaShell."), qPrintable(readme));
        QVERIFY(readme.contains("2. Copy ux0/data/twinsanity so that it is ux0:data/twinsanity."));
        QVERIFY(readme.contains("3. Copy your disc to ux0:data/twinsanity/Crash Twinsanity (Europe).iso."));
        QVERIFY(readme.contains("The mods take "));
        QVERIFY2(readme.contains("(musicpack test, arabic test)"), qPrintable(readme));   // the mods' names, in the overlay order
        QCOMPARE(r.names, (QStringList{"musicpack test", "arabic test"}));
        QVERIFY(readme.contains("Your saves are not copied"));
        // made again with nothing changed: every file kept
        VitaFolderReport again;
        QVERIFY2(VitaFolder::make(&m, game, folder, QString(), &again), qPrintable(again.error));
        QCOMPARE(again.copied, 0);
        QCOMPARE(again.kept, 5);
        QCOMPARE(again.removed, 0);
        // a mod switched off, another on: the copy follows (the one off leaves it)
        QVERIFY(ini.set("mods.musicpack.enabled", "off"));
        QVERIFY(ini.set("mods.other.enabled", "on"));
        QVERIFY(writeFile(data + "/mods/arabic/stray.txt", "left by hand"));   // a file the game folder's pack does not have
        VitaFolderReport third;
        QVERIFY2(VitaFolder::make(&m, game, folder, QString(), &third), qPrintable(third.error));
        QCOMPARE(third.mods, (QStringList{"arabic", "other"}));
        QVERIFY(!QFileInfo::exists(data + "/mods/musicpack"));
        QVERIFY(!QFileInfo::exists(data + "/mods/arabic/stray.txt"));
        QCOMPARE(readFile(data + "/mods/other/data/o.bin"), QByteArray("oooo"));
        QCOMPARE(third.removed, 2);   // the musicpack folder, the stray file
        QCOMPARE(IniSettings(data + "/ps2r.ini").get("mods.musicpack.enabled"), QStringLiteral("off"));
        // the disc's name on the console
        QCOMPARE(VitaFolder::consoleDiscName(&m, "/x/Crash: Twinsanity?.iso"), QStringLiteral("game.iso"));
        QCOMPARE(VitaFolder::consoleDiscName(&m, "C:\\Games\\Crash Twinsanity.ISO"), QStringLiteral("Crash Twinsanity.ISO"));
        QCOMPARE(VitaFolder::consoleDiscName(&m, "/x/Crash Twinsanity.img"), QStringLiteral("game.iso"));   // not one of the title's disc files
        QCOMPARE(VitaFolder::consoleDiscName(&m, QStringLiteral("/x/Crash Twinsanity (Fran") + QChar(0xE7) + QStringLiteral("ais).iso")), QStringLiteral("game.iso"));
        QCOMPARE(VitaFolder::consoleDiscName(&m, QString()), QStringLiteral("game.iso"));
        QCOMPARE(VitaFolder::consolePath("ux0/data/twinsanity"), QStringLiteral("ux0:data/twinsanity"));
        // a game folder with nothing in it yet: the header alone (the console's own defaults), game.iso, no mods; the
        // first disc on the Games page names the disc when the folder names none
        VitaFolderReport bare;
        const QString empty = home_.path() + "/Games/Nothing Here";
        QVERIFY2(VitaFolder::make(&m, empty, VitaFolder::folderIn(&m, empty), QString(), &bare), qPrintable(bare.error));
        QCOMPARE(readFile(bare.dataDir + "/ps2r.ini"), QByteArray("# test header\n"));
        QCOMPARE(readFile(bare.dataDir + "/disc-image.txt"), QByteArray("game.iso\n"));
        QVERIFY(bare.mods.isEmpty());
        QVERIFY(QString::fromUtf8(readFile(bare.folder + "/README-vita.txt")).contains("No mods are switched on"));
        VitaFolderReport named;
        QVERIFY(VitaFolder::make(&m, empty, VitaFolder::folderIn(&m, empty), disc, &named));
        QCOMPARE(readFile(named.dataDir + "/disc-image.txt"), QByteArray("Crash Twinsanity (Europe).iso\n"));
    }

    void manifest() {
        TitleManifest m;
        QString err;
        QVERIFY2(m.loadJson(QJsonDocument::fromJson(kManifest).object(), &err, "/titles"), qPrintable(err));
        QCOMPARE(m.id(), QStringLiteral("SLES-52568"));
        QCOMPARE(m.discManifests(), QStringList{"/titles/discs/pal.json"});
        QCOMPARE(m.playableExecutables(), QStringList{"SLES_525.68"});
        QCOMPARE(m.computers().size(), 2);
        QCOMPARE(m.computer("windows-x64")->exe, QStringLiteral("twinsanityevolved.exe"));
        QCOMPARE(m.targetLabel("vita"), QStringLiteral("PS Vita"));
        QCOMPARE(m.vitaNote().size(), 3);
        QCOMPARE(m.runArgs(), QStringList{"--trace-state"});
        QCOMPARE(m.allSettingsRows().size(), 5);
        QCOMPARE(m.allSettingsRows()[1].choices.size(), 2);   // a switch gets off/on
        QVERIFY(m.allSettingsRows()[0].writeDefault);
        QVERIFY(m.protectedNames().contains("memcards-mods"));
        QCOMPARE(m.builtinMods().size(), 1);
        QCOMPARE(m.scanMinBytes(), qint64(1024));
        TitleManifest bad;
        QVERIFY(!bad.loadJson(QJsonObject(), &err));
        QJsonObject noRelease = QJsonDocument::fromJson(kManifest).object();
        noRelease.remove("release");
        QVERIFY(!bad.loadJson(noRelease, &err));
        QVERIFY(err.contains("releases_url"));
    }

    void releaseChoice() {
        const QJsonArray releases = QJsonDocument::fromJson(R"([
          {"tag_name": "v0.4.0-rc1", "prerelease": true, "assets": [{"name": "twinsanityevolved-v0.4.0-rc1-macos-arm64.zip", "browser_download_url": "http:\/\/x/a"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s"}]},
          {"tag_name": "v0.3.1", "assets": [{"name": "twinsanityevolved-v0.3.1-windows-x64.zip", "browser_download_url": "http:\/\/x/w"}, {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s1"}]},
          {"tag_name": "v0.3.0", "assets": [{"name": "twinsanityevolved-v0.3.0-macos-arm64.zip", "size": 1234, "browser_download_url": "http:\/\/x/m", "url": "http:\/\/api/m"},
                                            {"name": "twinsanityevolved-v0.3.0-vita.vpk", "browser_download_url": "http:\/\/x/v"},
                                            {"name": "SHA256SUMS", "browser_download_url": "http:\/\/x/s0", "url": "http:\/\/api/s0"}]}])").array();
        ReleaseChoice c;
        // the prerelease is skipped; v0.3.1 has no macOS zip: v0.3.0 is the newest carrying it
        QVERIFY(ReleaseCatalogue::pick(releases, "twinsanityevolved-{tag}-{target}.zip", "macos-arm64", "SHA256SUMS", false, &c));
        QCOMPARE(c.tag, QStringLiteral("v0.3.0"));
        QCOMPARE(c.asset.url, QUrl("http://x/m"));
        QCOMPARE(c.asset.bytes, qint64(1234));
        QCOMPARE(c.sums.url, QUrl("http://x/s0"));
        QVERIFY(ReleaseCatalogue::pick(releases, "twinsanityevolved-{tag}-{target}.zip", "macos-arm64", "SHA256SUMS", true, &c));
        QCOMPARE(c.asset.url, QUrl("http://api/m"));   // a token: the API's asset URL
        QVERIFY(ReleaseCatalogue::pick(releases, "twinsanityevolved-{tag}-vita.vpk", "vita", "SHA256SUMS", false, &c));
        QCOMPARE(c.asset.name, QStringLiteral("twinsanityevolved-v0.3.0-vita.vpk"));
        QVERIFY(ReleaseCatalogue::pick(releases, "twinsanityevolved-{tag}-{target}.zip", "windows-x64", "SHA256SUMS", false, &c));
        QCOMPARE(c.tag, QStringLiteral("v0.3.1"));
        QVERIFY(!ReleaseCatalogue::pick(releases, "twinsanityevolved-{tag}-{target}.zip", "linux-aarch64", "SHA256SUMS", false, &c));
        const QString sha(64, 'a');
        const QString sums = sha + "  ./twinsanityevolved-v0.3.0-macos-arm64.zip\n" + QString(64, 'b') + " *other.zip\n" + QString(64, 'C') + "  plain.vpk\n";
        QCOMPARE(ReleaseCatalogue::shaFor(sums, "twinsanityevolved-v0.3.0-macos-arm64.zip"), sha);
        QCOMPARE(ReleaseCatalogue::shaFor(sums, "other.zip"), QString(64, 'b'));
        QCOMPARE(ReleaseCatalogue::shaFor(sums, "plain.vpk"), QString(64, 'c'));
        QVERIFY(ReleaseCatalogue::shaFor(sums, "missing.zip").isEmpty());
        QCOMPARE(ReleaseCatalogue::bytesText(18 * 1024 * 1024 + 100 * 1024), QStringLiteral("18.1 MB"));
        QCOMPARE(ReleaseCatalogue::rateText(3.2 * 1024 * 1024), QStringLiteral("3.2 MB/s"));
        QVERIFY(QStringList({"macos-arm64", "macos-x86_64", "windows-x64", "linux-x86_64", "linux-aarch64"}).contains(ReleaseCatalogue::hostTarget()));
    }

    void gameFolder() {
        const TitleManifest m = loadedManifest(home_.path());
        QCOMPARE(GameFolder::suggested(&m), home_.path() + "/Games/Crash Twinsanity");
        QCOMPARE(GameFolder::resolveChoice(home_.path() + "/Games", &m), home_.path() + "/Games/Crash Twinsanity");
        QCOMPARE(GameFolder::resolveChoice(home_.path() + "/Games/Crash Twinsanity (PS Vita)", &m), home_.path() + "/Games/Crash Twinsanity (PS Vita)");
        const QString dir = home_.path() + "/Games/Crash Twinsanity";
        const GameFolder gf(&m, dir);
        QString err;
        QVERIFY2(gf.prepare({{"video.frame_rate", "60"}, {"video.vsync", "off"}, {"paths.memcards", "memcards"}, {"paths.mods", "mods"}}, &err), qPrintable(err));
        QVERIFY(QFileInfo(dir + "/memcards").isDir());
        QVERIFY(QFileInfo(dir + "/mods").isDir());
        IniSettings ini(gf.settingsFile());
        QCOMPARE(ini.get("paths.memcards"), QStringLiteral("memcards"));
        QCOMPARE(ini.get("video.frame_rate"), QStringLiteral("60"));
        QFile f(gf.settingsFile());
        QVERIFY(f.open(QIODevice::ReadOnly));
        QVERIFY(f.readAll().startsWith("# test header\n"));
        f.close();
        // an existing value is never replaced by a default
        QVERIFY(ini.set("video.frame_rate", "120"));
        QVERIFY(gf.prepare({{"video.frame_rate", "60"}}, &err));
        QCOMPARE(ini.get("video.frame_rate"), QStringLiteral("120"));
        // folders: relative inside the game folder, absolute outside, read back absolute
        QCOMPARE(gf.folderSetting("paths.memcards", "memcards"), dir + "/memcards");
        QVERIFY(gf.setFolderSetting("paths.memcards", dir + "/saves/cards", &err));
        QCOMPARE(ini.get("paths.memcards"), QStringLiteral("saves/cards"));
        QCOMPARE(gf.folderSetting("paths.memcards", "memcards"), dir + "/saves/cards");
        QVERIFY(gf.setFolderSetting("paths.memcards", home_.path() + "/elsewhere", &err));
        QCOMPARE(ini.get("paths.memcards"), home_.path() + "/elsewhere");
        QCOMPARE(gf.folderSetting("paths.memcards", "memcards"), home_.path() + "/elsewhere");
        // the disc the game reads
        QVERIFY(gf.disc().isEmpty());
        QVERIFY(gf.setDisc(home_.path() + "/Downloads/Crash Twinsanity.iso", &err));
        QCOMPARE(gf.disc(), home_.path() + "/Downloads/Crash Twinsanity.iso");
        QVERIFY(GameFolder::holdsGame(dir, &m));
        QVERIFY(GameFolder::writable(home_.path() + "/Games/not/yet/made"));
        QVERIFY(GameFolder::freeBytes(dir) > 0);
    }

    // an install adds the two language keys to a NEW folder's ps2r.ini, but not beside an older folder's game.language
    // (their defaults would win over the player's earlier choice; the game reads game.language as both until a row changes)
    void olderFolderKeepsItsLanguage() {
        QJsonObject root = QJsonDocument::fromJson(kManifest).object();
        QJsonObject settings = root.value("settings").toObject();
        QJsonArray groups = settings.value("groups").toArray();
        QJsonArray rows;
        rows << QJsonObject{{"key", "game.text_language"}, {"label", "Text language"}, {"kind", "choice"}, {"default", "english"},
                            {"write_default", true}, {"choices_from", "text_languages"}}
             << QJsonObject{{"key", "game.speech_language"}, {"label", "Speech language"}, {"kind", "choice"}, {"default", "english"},
                            {"write_default", true}, {"choices_from", "speech_languages"}};
        groups.prepend(QJsonObject{{"title", "Game"}, {"rows", rows}});
        settings["groups"] = groups;
        root["settings"] = settings;
        QDir().mkpath(home_.path() + "/lang-titles");
        QFile mf(home_.path() + "/lang-titles/title.json");
        QVERIFY(mf.open(QIODevice::WriteOnly));
        mf.write(QJsonDocument(root).toJson());
        mf.close();
        LauncherApp app;
        QString err;
        QVERIFY2(app.loadManifest(mf.fileName(), &err), qPrintable(err));
        // a derived row offers its default until a disc says more
        bool found = false;
        for (const SettingsRowSpec& r : app.manifest()->allSettingsRows())
            if (r.key == "game.text_language") {
                found = true;
                QCOMPARE(r.choicesFrom, QStringLiteral("text_languages"));
                QCOMPARE(r.choices.size(), 1);
                QCOMPARE(r.choices.first().value, QStringLiteral("english"));
                QCOMPARE(r.choices.first().label, QStringLiteral("English"));
            }
        QVERIFY(found);
        auto keys = [](const QVector<QPair<QString, QString>>& d) {
            QStringList k;
            for (const auto& p : d) k << p.first + "=" + p.second;
            return k;
        };
        const QString fresh = home_.path() + "/lang-fresh/ps2r.ini";
        QVERIFY(keys(app.settingDefaultsFor(fresh)).contains("game.text_language=english"));
        QVERIFY(keys(app.settingDefaultsFor(fresh)).contains("game.speech_language=english"));
        IniSettings older(home_.path() + "/lang-older/ps2r.ini");
        QVERIFY(older.set("game.language", "portuguese"));
        const QStringList kept = keys(app.settingDefaultsFor(older.path()));
        QVERIFY(!kept.contains("game.text_language=english") && !kept.contains("game.speech_language=english"));
        QVERIFY(kept.contains("video.frame_rate=60"));   // the other rows are added as before
        GameFolder gf(app.manifest(), home_.path() + "/lang-older");
        QVERIFY2(gf.prepare(app.settingDefaultsFor(gf.settingsFile()), &err), qPrintable(err));
        QCOMPARE(older.get("game.language"), QStringLiteral("portuguese"));
        QVERIFY(!older.has("game.text_language") && !older.has("game.speech_language"));
    }

    // the PS Vita package from a LOCAL release list (a hand test's: a GitHub-shaped releases.json naming its files by
    // file:// URL, SHA256SUMS beside them): the launcher's PS Vita step takes the package from the local file, checks it
    // against the list's SHA256SUMS and places it -- no socket, no server
    void installVitaFromAFileList() {
        const QTemporaryDir kit;
        QVERIFY(kit.isValid());
        const QString rel = kit.path() + "/release";
        const QByteArray vpk = QByteArray("PK fake package bytes ") + QByteArray(4096, 'v');
        const QString vpkName = "twinsanityevolved-v0.9.0-vita.vpk";
        QVERIFY(writeFile(rel + "/" + vpkName, vpk));
        const QByteArray sum = QCryptographicHash::hash(vpk, QCryptographicHash::Sha256).toHex();
        QVERIFY(writeFile(rel + "/SHA256SUMS", sum + "  " + vpkName.toUtf8() + "\n"));
        const QString base = QUrl::fromLocalFile(rel).toString();
        const QJsonArray assets{QJsonObject{{"name", vpkName}, {"size", double(vpk.size())}, {"browser_download_url", base + "/" + vpkName},
                                            {"url", base + "/" + vpkName}},
                                QJsonObject{{"name", "SHA256SUMS"}, {"size", double(sum.size() + 2 + vpkName.size() + 1)},
                                            {"browser_download_url", base + "/SHA256SUMS"}, {"url", base + "/SHA256SUMS"}}};
        const QJsonArray releases{QJsonObject{{"tag_name", "v0.9.0"}, {"name", "kit"}, {"draft", false}, {"prerelease", false},
                                              {"published_at", "2026-10-27T00:00:00Z"}, {"assets", assets}}};
        QVERIFY(writeFile(rel + "/releases.json", QJsonDocument(releases).toJson()));
        const TitleManifest m = loadedManifest(home_.path());
        AppData data("TwinsanityEvolved-filelist-test");
        data.setValue("releases_url", base + "/releases.json");
        ReleaseInstaller inst(&data, &m);
        QSignalSpy done(&inst, &ReleaseInstaller::finished);
        const QString folder = home_.path() + "/Games/File List Test/vita";
        inst.install("vita", folder);
        QVERIFY(done.wait(20000));
        QVERIFY2(done.first().first().toBool(), qPrintable(inst.failureText()));
        QCOMPARE(inst.tag(), QStringLiteral("v0.9.0"));
        QCOMPARE(readFile(folder + "/twinsanityevolved.vpk"), vpk);
        // a damaged local file: refused like a download
        QVERIFY(writeFile(rel + "/" + vpkName, vpk + "x"));
        done.clear();
        inst.install("vita", home_.path() + "/Games/File List Damaged/vita");
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toBool(), false);
        QVERIFY(!QFileInfo::exists(home_.path() + "/Games/File List Damaged/vita/twinsanityevolved.vpk"));
    }

    void installEndToEnd() {
        const TitleManifest m = loadedManifest(home_.path());
        AppData data("TwinsanityEvolved");
        FixtureReleaseServer server;
        QVERIFY(server.start());
        data.setValue("releases_url", server.releasesUrl());
        const QString target = "macos-arm64";
        const QByteArray zip = makeZip("twinsanityevolved-macos-arm64",
                                       {{"twinsanityevolved", "#!/bin/sh\necho game\n"}, {"README.md", "readme"}, {"ps2r.ini", "game.language = french\n"}},
                                       "twinsanityevolved");
        server.publish("v0.3.0", {{"twinsanityevolved-v0.3.0-macos-arm64.zip", zip}, {"twinsanityevolved-v0.3.0-vita.vpk", "vpk bytes"}});
        const QString folder = home_.path() + "/Games/Install Test";
        QDir().mkpath(folder);
        IniSettings mine(folder + "/ps2r.ini");
        QVERIFY(mine.set("game.language", "german"));   // the user's file must survive the install
        ReleaseInstaller inst(&data, &m);
        QSignalSpy done(&inst, &ReleaseInstaller::finished);
        inst.install(target, folder);
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toBool(), true);
        QCOMPARE(inst.tag(), QStringLiteral("v0.3.0"));
        QCOMPARE(inst.placedFile(), folder + "/twinsanityevolved");
#ifndef Q_OS_WIN   // (Windows has no executable bit: a file is executable by its extension)
        QVERIFY(QFileInfo(folder + "/twinsanityevolved").isExecutable());
#endif
        QVERIFY(QFileInfo::exists(folder + "/README.md"));
        QCOMPARE(mine.get("game.language"), QStringLiteral("german"));   // a protected name in the zip is never written
        QCOMPARE(inst.received(), qint64(zip.size()));
        QVERIFY(inst.downloadedBytes() >= zip.size());
        QVERIFY(!QFileInfo::exists(data.downloadsDir() + "/twinsanityevolved-v0.3.0-macos-arm64.zip"));   // the download is not kept
        // the PS Vita's package
        done.clear();
        inst.install("vita", home_.path() + "/Games/Vita Test");
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toBool(), true);
        QFile vpk(home_.path() + "/Games/Vita Test/twinsanityevolved.vpk");
        QVERIFY(vpk.open(QIODevice::ReadOnly));
        QCOMPARE(vpk.readAll(), QByteArray("vpk bytes"));
        // no release carries this target
        done.clear();
        inst.install("windows-x64", folder);
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toBool(), false);
        QCOMPARE(inst.failure(), ReleaseInstaller::NoRelease);
        QVERIFY(inst.failureText().startsWith("There is no download for Windows yet."));
        // a damaged download: SHA256SUMS names other bytes -- refused, deleted, nothing placed
        server.publish("v0.3.1", {{"twinsanityevolved-v0.3.1-macos-arm64.zip", zip}});
        server.corrupt("twinsanityevolved-v0.3.1-macos-arm64.zip");
        done.clear();
        inst.install(target, home_.path() + "/Games/Damaged");
        QVERIFY(done.wait(20000));
        QCOMPARE(done.first().first().toBool(), false);
        QCOMPARE(inst.failure(), ReleaseInstaller::Damaged);
        QCOMPARE(inst.failureText(), QStringLiteral("The download was damaged and has been deleted. Try again."));
        QVERIFY(!QFileInfo::exists(home_.path() + "/Games/Damaged/twinsanityevolved"));
        QVERIFY(!QFileInfo::exists(data.downloadsDir() + "/twinsanityevolved-v0.3.1-macos-arm64.zip"));
        server.stop();
        // offline: the server is gone
        done.clear();
        inst.install(target, home_.path() + "/Games/Offline");
        QVERIFY(done.wait(40000));
        QCOMPARE(done.first().first().toBool(), false);
        QCOMPARE(inst.failure(), ReleaseInstaller::Offline);
        QCOMPARE(inst.failureText(), QStringLiteral("The download did not finish. Check your connection and try again."));
        QVERIFY(!QFileInfo::exists(home_.path() + "/Games/Offline/twinsanityevolved"));
    }

    void quitDuringDownload() {
        const TitleManifest m = loadedManifest(home_.path());
        AppData data("TwinsanityEvolved");
        FixtureReleaseServer server;
        QVERIFY(server.start());
        data.setValue("releases_url", server.releasesUrl());
        QByteArray big(300 * 1024, '\0');
        for (int i = 0; i < big.size(); ++i) big[i] = char((i * 2654435761u) >> 13);
        server.publish("v0.5.0", {{"twinsanityevolved-v0.5.0-vita.vpk", big}});
        server.setRateLimit(20 * 1024);
        auto* inst = new ReleaseInstaller(&data, &m);
        inst->install("vita", home_.path() + "/Games/Quit Test");
        QVERIFY(QTest::qWaitFor([&] { return inst->phase() == ReleaseInstaller::Downloading && inst->received() > 0; }, 20000));
        delete inst;   // the launcher quits mid-download: no failure path on a destroyed object, nothing placed
        QTest::qWait(300);
        QVERIFY(!QFileInfo::exists(home_.path() + "/Games/Quit Test/twinsanityevolved.vpk"));
    }

    void iniRoundTrip() {
        QTemporaryDir dir;
        IniSettings ini(dir.path() + "/ps2r.ini");
        QVERIFY(!ini.exists());
        QCOMPARE(ini.get("video.frame_rate", "60"), QStringLiteral("60"));
        QString err;
        QVERIFY2(ini.set("video.frame_rate", "120", &err), qPrintable(err));
        QVERIFY(ini.set("game.language", "english", &err));
        QCOMPARE(ini.get("video.frame_rate"), QStringLiteral("120"));
        QVERIFY(ini.set("video.frame_rate", "uncapped", &err));
        QCOMPARE(ini.get("video.frame_rate"), QStringLiteral("uncapped"));
        QCOMPARE(ini.keys(), QStringList({"video.frame_rate", "game.language"}));
        const QString text = "[video]\nvsync = on   # the presenter\n";
        QString v;
        QVERIFY(IniSettings::getFromText(text, "video.vsync", &v));
        QCOMPARE(v, QStringLiteral("on"));
        const QString next = IniSettings::setInText(text, "video.vsync", "off");
        QVERIFY(IniSettings::getFromText(next, "video.vsync", &v));
        QCOMPARE(v, QStringLiteral("off"));
        QVERIFY(next.startsWith("[video]\n"));
    }

    void libraryRemembersRemovals() {
        AppData data("TwinsanityEvolved-library-test");
        GameLibrary lib(&data);
        DiscFound d;
        d.path = home_.path() + "/Downloads/a.iso";
        d.accepted = true;
        d.fromSearch = true;
        QVERIFY(lib.add(d));
        lib.remove(d.path);                 // the player's Remove
        QVERIFY(!lib.add(d));               // a later search does not bring it back
        QVERIFY(!lib.game(d.path));
        d.fromSearch = false;
        QVERIFY(lib.add(d));                // choosing it again does
        lib.remove(d.path, false);          // not the player's (a changed setup choice)
        d.fromSearch = true;
        QVERIFY(lib.add(d));
        GameLibrary reloaded(&data);        // persisted in launcher.json
        QVERIFY(reloaded.game(d.path));
    }

    void libraryTimes() {
        const QDateTime now(QDate(2026, 10, 19), QTime(12, 0));
        QCOMPARE(GameLibrary::relativeTime(QDateTime(), now), QStringLiteral("Never played"));
        QCOMPARE(GameLibrary::relativeTime(now.addSecs(-30), now), QStringLiteral("Last played just now"));
        QCOMPARE(GameLibrary::relativeTime(now.addSecs(-600), now), QStringLiteral("Last played 10 min ago"));
        QCOMPARE(GameLibrary::relativeTime(now.addDays(-1), now), QStringLiteral("Last played yesterday 12:00"));
        QCOMPARE(GameLibrary::playtimeText(59), QStringLiteral("under a minute"));
        QCOMPARE(GameLibrary::playtimeText(22320), QStringLiteral("6 h 12 min"));
        QCOMPARE(GeneratedArtwork::initialsOf("Crash Twinsanity"), QStringLiteral("CT"));
    }

    // what a disc's card says for every verdict, and the reason another release does not play (the region table's)
    void discWords() {
        DiscFound d;
        d.region = "PAL";
        d.verdict = "modified";
        d.checking = true;
        QCOMPARE(DiscText::identityBadge(d), QStringLiteral("PAL · checking…"));
        QCOMPARE(DiscText::verdictLine(d), QStringLiteral("Checking whether this is the original disc…"));
        d.checking = false;
        d.imageBytes = 2579234816ll;
        d.dumpBytes = 2558263296ll;
        QCOMPARE(DiscText::identityBadge(d), QStringLiteral("PAL · modified disc"));
        QCOMPARE(DiscText::identityTone(d), Badge::Accent);
        QCOMPARE(DiscText::verdictLine(d), QStringLiteral("A modified copy of the PAL disc: 20 MB larger than the original."));
        d.imageBytes = d.dumpBytes;
        QCOMPARE(DiscText::verdictLine(d), QStringLiteral("A modified copy of the PAL disc: the size of the original, with other contents."));
        d.differences = QStringList{"CRASH6/CRASH.BH", "CRASH6/CRASH.BD", "SYSTEM.CNF", "FMV/ATTRACT.PSS"};
        QCOMPARE(DiscText::verdictLine(d), QStringLiteral("A modified copy of the PAL disc. These files differ from the original: CRASH6/CRASH.BH, CRASH6/CRASH.BD, SYSTEM.CNF and 1 more."));
        d.verdict = "retail";
        QCOMPARE(DiscText::identityBadge(d), QStringLiteral("PAL · original disc"));
        QCOMPARE(DiscText::identityTone(d), Badge::Ok);
        QVERIFY(DiscText::verdictLine(d).isEmpty());
        QVERIFY(DiscText::stateBadge(d, true).isEmpty());
        d.verdict = "unknown";
        QCOMPARE(DiscText::identityBadge(d), QStringLiteral("PAL · unrecognised version"));
        QCOMPARE(DiscText::identityTone(d), Badge::Warn);
        DiscFound us;
        us.region = "NTSC-U";
        us.serial = "SLUS-20909";
        us.verdict = "retail";
        QCOMPARE(DiscText::identityBadge(us), QStringLiteral("NTSC-U · original disc"));
        QCOMPARE(DiscText::stateBadge(us, false), QStringLiteral("Not playable in this version"));
        const QString reason = QString::fromStdString(ps2r::app::RegionNotPlayable(us.serial.toStdString()));
        QVERIFY(reason.startsWith("This is the North American disc (SLUS-20909). The port runs the European game's code"));
        QCOMPARE(DiscText::stateLine(us, false, reason), reason);
        // an older launcher's identity ("retail" meant a file list matched): never trusted, the disc is checked again
        QJsonObject old{{"size", 10.0}, {"modified", 5.0}, {"accepted", true}, {"region", "PAL"}, {"retail", true}, {"known", true}};
        QVERIFY(DiscScanner::stale(old));
        const DiscFound o = DiscScanner::fromJson(old);
        QVERIFY(!o.retail && o.verdict.isEmpty());
        QCOMPARE(DiscText::identityBadge(o), QStringLiteral("PAL · checking…"));
        QVERIFY(!DiscScanner::stale(DiscScanner::toJson(d)));
    }

    // the verdict is cached by path, size and modification time: the second look at the same file hashes nothing
    void identityCache() {
        ps2r::app::ClearRetailManifests();
        ps2r::app::ClearKnownDumps();
        QByteArray elf(6000, '\x5A');
        const char head[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
        elf.replace(0, 7, QByteArray(head, 7));
        elf[18] = 8;
        const QString path = home_.path() + "/cache-test.iso";
        QFile f(path);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(tinyIso({{"SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.01\r\nVMODE = PAL\r\n"}, {"SLES_525.68", elf}}));
        f.close();
        DiscScanner first;
        QSignalSpy found1(&first, &DiscScanner::found), checked1(&first, &DiscScanner::checked);
        first.identify(path);
        QVERIFY(checked1.wait(20000));
        QCOMPARE(found1.size(), 1);
        QVERIFY(found1.first().first().value<DiscFound>().checking);   // the card first says "checking…"
        const DiscFound c = checked1.first().first().value<DiscFound>();
        QVERIFY(c.checked && !c.checking && c.imageSha1.size() == 40 && c.imageCrc32.size() == 8);
        QCOMPARE(c.verdict, QStringLiteral("unknown"));   // no table of known dumps here: a program the launcher does not know
        const QJsonObject cache = first.cache();
        QVERIFY(cache.contains(QFileInfo(path).absoluteFilePath()));
        QCOMPARE(cache.value(QFileInfo(path).absoluteFilePath()).toObject().value("image_sha1").toString(), c.imageSha1);
        DiscScanner second;
        second.setCache(cache);
        QSignalSpy found2(&second, &DiscScanner::found), checked2(&second, &DiscScanner::checked);
        second.identify(path);
        QVERIFY(found2.wait(20000));
        QTest::qWait(300);
        const DiscFound again = found2.first().first().value<DiscFound>();
        QVERIFY(again.checked && !again.checking && again.imageSha1 == c.imageSha1);
        QCOMPARE(checked2.size(), 0);   // nothing was hashed again
        // the file changes (size): the cache no longer applies
        QVERIFY(f.open(QIODevice::Append));
        f.write(QByteArray(2048, '\0'));
        f.close();
        DiscScanner third;
        third.setCache(cache);
        QSignalSpy checked3(&third, &DiscScanner::checked);
        third.identify(path);
        QVERIFY(checked3.wait(20000));
    }

    // a REAL disc image (PS2R_IDENTITY_ISO) identified with the real title's tables (PS2R_IDENTITY_TITLE: launcher/title.json):
    // the card's words, the verdict and the image's hashes into PS2R_IDENTITY_REPORT; with PS2R_IDENTITY_BADGE the badge must
    // be that one (the disc identity judge's real NTSC-U case). Skipped without a disc: no game data in the tests.
    void realDiscIdentity() {
        const QString iso = qEnvironmentVariable("PS2R_IDENTITY_ISO");
        const QString title = qEnvironmentVariable("PS2R_IDENTITY_TITLE");
        if (iso.isEmpty() || title.isEmpty()) QSKIP("PS2R_IDENTITY_ISO / PS2R_IDENTITY_TITLE not set");
        TitleManifest m;
        QString err;
        QFile tf(title);
        QVERIFY(tf.open(QIODevice::ReadOnly));
        QVERIFY2(m.loadJson(QJsonDocument::fromJson(tf.readAll()).object(), &err, QFileInfo(title).absolutePath()), qPrintable(err));
        ps2r::app::ClearRetailManifests();
        ps2r::app::ClearKnownDumps();
        for (const QString& list : m.discManifests()) {
            std::string e;
            QVERIFY2(ps2r::app::LoadDiscTable(QFile::encodeName(list).toStdString(), &e), e.c_str());
        }
        DiscScanner scanner;
        QSignalSpy found(&scanner, &DiscScanner::found), checked(&scanner, &DiscScanner::checked);
        scanner.identify(iso);
        QVERIFY(found.wait(60000));
        const DiscFound quick = found.first().first().value<DiscFound>();
        QVERIFY(checked.wait(600000));
        const DiscFound d = checked.first().first().value<DiscFound>();
        const bool playable = m.playableExecutables().isEmpty() || m.playableExecutables().contains(d.executable, Qt::CaseInsensitive);
        const std::string region = ps2r::app::RegionNotPlayable(d.serial.toStdString());
        const QString reason = region.empty() ? m.playsNote() : QString::fromStdString(region);
        QJsonObject r{{"first_badge", DiscText::identityBadge(quick)}, {"badge", DiscText::identityBadge(d)},
                      {"tone", int(DiscText::identityTone(d))}, {"line", DiscText::verdictLine(d)}, {"verdict", d.verdict},
                      {"region", d.region}, {"serial", d.serial}, {"executable", d.executable}, {"volume_id", d.volumeId},
                      {"retail", d.retail}, {"image_crc32", d.imageCrc32}, {"image_sha1", d.imageSha1},
                      {"image_bytes", double(d.imageBytes)}, {"dump_version", d.dumpVersion}, {"playable", playable},
                      {"state_badge", DiscText::stateBadge(d, playable)},
                      {"state_line", DiscText::stateLine(d, playable, reason)}};
        const QString out = qEnvironmentVariable("PS2R_IDENTITY_REPORT");
        if (!out.isEmpty()) {
            QFile f(out);
            QVERIFY(f.open(QIODevice::WriteOnly));
            f.write(QJsonDocument(r).toJson());
        }
        const QString want = qEnvironmentVariable("PS2R_IDENTITY_BADGE");
        if (!want.isEmpty()) QCOMPARE(DiscText::identityBadge(d), want);
    }

    void appDataPaths() {
        AppData a("TwinsanityEvolved");
#ifdef Q_OS_MACOS
        QCOMPARE(a.root(), home_.path() + "/Library/Application Support/TwinsanityEvolved");
#endif
        QCOMPARE(a.expand("~/.te/x"), home_.path() + "/.te/x");
        QCOMPARE(a.expand("${app_data}/Logs"), a.root() + "/Logs");
        QCOMPARE(AppData::shortPath(home_.path() + "/Downloads"), QStringLiteral("~/Downloads"));
    }
};

QTEST_MAIN(TestLauncherModels)
#include "test_launcher_models.moc"
