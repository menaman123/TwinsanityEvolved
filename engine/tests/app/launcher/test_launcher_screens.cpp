// tests/app/launcher/test_launcher_screens.cpp -- every screen of the launcher, offscreen, in the order a player meets
// them: the setup's four steps with their empty and error states (no disc found, a refused file, a folder that cannot be
// written, the PS Vita note, another computer, a download that cannot finish, the download in progress, done), then
// Games (ready, an update out, a newer launcher offered, the empty state), Mods (empty, a mod, importing, refused, remove,
// replace, a North American mod, the PS Vita panel), Settings, About (the launcher's update, the PS Vita section); no
// placeholder text or emoji
// anywhere, no developer vocabulary in any visible text.
//
// By default everything is GENERATED (a tiny ISO 9660 image whose retail list the test writes; a release served by
// FixtureReleaseServer on 127.0.0.1; a table of known dumps naming the generated original image): no disc and no network.
// PS2R_SCREENS_MANIFEST (the repo's launcher/title.json) and PS2R_SCREENS_ISO (a disc image, linked into the test's own
// home folder -- a MODIFIED PAL image, so its one card reads "PAL · modified disc") make the same screens with the real title;
// PS2R_SCREENS_ISO2 (a retail North American image) adds the Games page with both discs' cards (launcher-games-two-discs).
// PS2R_SCREENS_OUT=DIR writes one PNG
// per screen as launcher-<screen>.png (docs/launcher/screenshots/).
#include <QtTest/QtTest>
#include <QDirIterator>
#include <QScrollArea>
#include <QScrollBar>
#include <QBuffer>
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QRegularExpression>
#include <QTemporaryDir>
#if __has_include(<QtCore/private/qzipwriter_p.h>)
#include <QtCore/private/qzipwriter_p.h>
#else
#include <QtGui/private/qzipwriter_p.h>
#endif
#include "about_page.h"
#include "app_data.h"
#include "choice_card.h"
#include "details_sheet.h"
#include "disc_choice_card.h"
#include "empty_state.h"
#include "fixture_release_server.h"
#include "flat_button.h"
#include "folder_row.h"
#include "game_card.h"
#include "game_library.h"
#include "game_process.h"
#include "ini_settings.h"
#include "games_page.h"
#include "launcher_app.h"
#include "launcher_style.h"
#include "launcher_theme.h"
#include "launcher_update_box.h"
#include "main_window.h"
#include "mod_row.h"
#include "mods_page.h"
#include "progress_panel.h"
#include "sheet_overlay.h"
#include "release_catalogue.h"
#include "release_installer.h"
#include "segmented_control.h"
#include "self_update.h"
#include "settings_page.h"
#include "settings_row.h"
#include "setup_disc_step.h"
#include "setup_folder_step.h"
#include "setup_install_step.h"
#include "setup_sheet.h"
#include "setup_target_step.h"
#include "sidebar.h"
#include "switch_control.h"
#include "title_manifest.h"
#include "app/host/disc_image_hash.h"
using namespace ps2r::launcher;

namespace {
// --- a generated ISO 9660 volume: PVD at 16, terminator at 17, the root directory at 18, files from 20 ---
void both32(QByteArray& b, int at, quint32 v) {
    for (int i = 0; i < 4; ++i) { b[at + i] = char(v >> (8 * i)); b[at + 7 - i] = char(v >> (8 * i)); }
}
void both16(QByteArray& b, int at, quint16 v) {
    b[at] = char(v); b[at + 1] = char(v >> 8); b[at + 2] = char(v >> 8); b[at + 3] = char(v);
}
QByteArray record(quint32 lba, quint32 size, bool dir, const QByteArray& id) {
    QByteArray r(33 + id.size() + ((33 + id.size()) & 1), '\0');
    r[0] = char(r.size());
    both32(r, 2, lba);
    both32(r, 10, size);
    r[25] = dir ? 2 : 0;
    both16(r, 28, 1);
    r[32] = char(id.size());
    r.replace(33, id.size(), id);
    return r;
}
QByteArray makeIso(const QVector<QPair<QString, QByteArray>>& files) {
    quint32 next = 20;
    QVector<quint32> lbas;
    for (const auto& f : files) { lbas << next; next += quint32(qMax<qsizetype>(1, (f.second.size() + 2047) / 2048)); }
    QByteArray img(int(next) * 2048, '\0');
    QByteArray root = record(18, 2048, true, QByteArray(1, '\0')) + record(18, 2048, true, QByteArray(1, '\1'));
    for (int i = 0; i < files.size(); ++i) {
        root += record(lbas[i], quint32(files[i].second.size()), false, (files[i].first + ";1").toLatin1());
        img.replace(int(lbas[i]) * 2048, files[i].second.size(), files[i].second);
    }
    img.replace(18 * 2048, root.size(), root);
    QByteArray pvd(2048, '\0');
    pvd[0] = 1;
    pvd.replace(1, 5, "CD001");
    pvd[6] = 1;
    pvd.replace(40, 32, QByteArray("CRASH").leftJustified(32, ' '));
    both32(pvd, 80, next);
    both16(pvd, 128, 2048);
    const QByteArray rootRec = record(18, 2048, true, QByteArray(1, '\0'));
    pvd.replace(156, rootRec.size(), rootRec);
    img.replace(16 * 2048, 2048, pvd);
    QByteArray term(2048, '\0');
    term[0] = static_cast<char>(-1);   // 0xFF: the set terminator's type
    term.replace(1, 5, "CD001");
    img.replace(17 * 2048, 2048, term);
    return img;
}
QByteArray pattern(int n, quint32 seed) {
    QByteArray b(n, '\0');
    quint32 x = seed * 2654435761u + 1;
    for (int i = 0; i < n; ++i) { x = x * 1103515245u + 12345u; b[i] = char(x >> 16); }
    return b;
}
QVector<QPair<QString, QByteArray>> discFiles() {
    QByteArray elf = pattern(6000, 2);
    const char head[] = {0x7F, 'E', 'L', 'F', 1, 1, 1};
    elf.replace(0, 7, QByteArray(head, 7));
    elf[18] = 8;
    elf[24] = 0x08; elf[25] = 0x00; elf[26] = 0x10; elf[27] = 0x00;
    return {{"SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.01\r\nVMODE = PAL\r\n"}, {"SLES_525.68", elf},
            {"ARCHIVE.BH", pattern(3000, 3)}, {"ARCHIVE.BD", pattern(90000, 4)}};
}
QString retailListJson(const QVector<QPair<QString, QByteArray>>& files) {
    QJsonArray rows;
    for (const auto& f : files) {
        QJsonObject r;
        r["path"] = f.first;
        r["bytes"] = double(f.second.size());
        r["sha256"] = QString::fromLatin1(QCryptographicHash::hash(f.second, QCryptographicHash::Sha256).toHex());
        r["core"] = true;
        rows << r;
    }
    QJsonObject o{{"schema", 2}, {"kind", "files"}, {"id", "test-pal"}, {"title", "Crash Twinsanity"}, {"region", "PAL"}, {"serial", "SLES-52568"},
                  {"executable", "SLES_525.68"}, {"volume_id", "CRASH"}, {"retail", false}, {"provenance", "the generated test disc"}, {"files", rows}};
    return QString::fromUtf8(QJsonDocument(o).toJson());
}
// The table of known dumps for the generated test: the generated original image IS the one known dump (size, CRC-32, SHA-1)
// and its program the known retail executable, so it is the original disc and the image with one byte changed (the same
// program) a modified one.
QString dumpTableJson(const QByteArray& image, const QByteArray& program) {
    QJsonObject d{{"id", 4514}, {"title", "Crash Twinsanity"}, {"serial", "SLES-52568"}, {"region", "PAL"}, {"version", "1.01"},
                  {"edition", "Original"}, {"bytes", double(image.size())},
                  {"crc32", QString::fromStdString(ps2r::app::Crc32::Hex(ps2r::app::Crc32::Of(image.constData(), size_t(image.size()))))},
                  {"sha1", QString::fromLatin1(QCryptographicHash::hash(image, QCryptographicHash::Sha1).toHex())}};
    QJsonObject p{{"serial", "SLES-52568"}, {"executable", "SLES_525.68"}, {"bytes", double(program.size())},
                  {"sha256", QString::fromLatin1(QCryptographicHash::hash(program, QCryptographicHash::Sha256).toHex())}};
    QJsonObject o{{"schema", 1}, {"kind", "redump"}, {"dumps", QJsonArray{d}}, {"programs", QJsonArray{p}}};
    return QString::fromUtf8(QJsonDocument(o).toJson());
}
const char* kManifest = R"JSON({
  "schema": 2, "id": "SLES-52568", "name": "Crash Twinsanity", "product": "TwinsanityEvolved", "app_data_folder": "TwinsanityEvolved",
  "binary_name": "twinsanityevolved", "description": "Crash Twinsanity running natively on your computer.",
  "disc": { "manifests": ["discs/test.json", "discs/test-dumps.json"], "plays": ["SLES_525.68"], "plays_note": "This version of the game plays the PAL disc (SLES-52568).", "extensions": [".iso"] },
  "release": { "releases_url": "http:\/\/127.0.0.1:9/releases", "page_url": "https:\/\/example.invalid/project", "zip_asset": "twinsanityevolved-{tag}-{target}.zip",
               "vita_asset": "twinsanityevolved-{tag}-vita.vpk", "sums_asset": "SHA256SUMS", "install_bytes": 52428800,
               "launcher_assets": { "macos-arm64": "twinsanityevolved-launcher-{tag}-macos-arm64.zip", "macos-x86_64": "twinsanityevolved-launcher-{tag}-macos-x86_64.zip",
                                    "windows-x64": "twinsanityevolved-launcher-windows-x64.zip", "linux-x86_64": "twinsanityevolved-launcher-linux-x86_64.AppImage" } },
  "targets": { "computers": [ { "id": "windows-x64", "label": "Windows", "short": "Windows", "exe": "twinsanityevolved.exe" },
                              { "id": "macos-arm64", "label": "macOS (Apple silicon)", "short": "macOS", "exe": "twinsanityevolved" },
                              { "id": "macos-x86_64", "label": "macOS (Intel)", "short": "macOS", "exe": "twinsanityevolved" },
                              { "id": "linux-x86_64", "label": "Linux (x86-64)", "short": "Linux", "exe": "twinsanityevolved" },
                              { "id": "linux-aarch64", "label": "Linux (ARM)", "short": "Linux", "exe": "twinsanityevolved" } ],
               "vita": { "label": "PS Vita", "file": "twinsanityevolved.vpk",
                         "note": ["Copy {vpk} to the PS Vita and install it with VitaShell.", "Copy the folder {data_dir} from the PS Vita folder to the PS Vita's memory card, so that it is {data} there: the settings, the mods and disc-image.txt.", "Copy your own disc image to {data}/{disc} on the PS Vita. The launcher never copies the disc image.", "The PS Vita needs its shader library ur0:data/libshacccg.suprx -- the usual homebrew step. The launcher never downloads or carries this file."],
                         "discs_note": ["{disc_file} names the disc the game starts: the launcher writes it for the disc you chose (Settings > Disc image), so copy that disc to {data} under the name it gives, {disc}.", "The European and the North American discs can live side by side on the PS Vita, for example as game.iso and ntsc.iso: write the name of the one to play in {data}/{disc_file}. The North American disc runs at 60 Hz and keeps its own memory card."],
                         "mods_note": "On the PS Vita, mods are switched here and copied: after you add or switch a mod, press Make the PS Vita folder again (here or in Settings), then copy {data_dir} to the PS Vita's memory card.",
                         "about": ["Choose PS Vita with Get the game for PS Vita or another computer on the Games page: the launcher puts the PS Vita version in a folder named vita inside your game folder.", "Install {vpk} from that folder with VitaShell; the PS Vita also needs ur0:data/libshacccg.suprx, which the launcher never carries.", "Copy the folder's {data_dir} to the PS Vita's memory card: it holds your settings, the language packs, the mods you switched on and {disc_file}.", "Copy your own disc image to {data} under the name {disc_file} gives; the European and the North American discs can sit side by side, as game.iso and ntsc.iso, and {disc_file} chooses between them.", "Mods are added and switched here on a computer, never on the PS Vita: after a change, press Make the PS Vita folder again and copy {data_dir} once more.", "Your saves stay on the PS Vita, in {data}/memcards; the North American disc keeps its own card beside it."],
                         "folder": { "name": "vita", "data_dir": "ux0/data/twinsanity", "readme": "README-vita.txt", "default_disc": "game.iso",
                                     "settings_left_out": ["paths.memcards", "paths.mods", "game.hz", "video.scale", "video.fullscreen", "video.vsync", "video.frame_rate"] } } },
  "run": { "iso_flag": "--iso", "config_flag": "--config", "args": ["--trace-state"], "title_state_line": "game state @ boundary", "title_state_name": "Title (7)" },
  "game_folder": { "default": "~/Games/Crash Twinsanity", "settings": "ps2r.ini", "memcards": "memcards", "mods": "mods", "disc_file": "disc-image.txt",
                   "protected": ["ps2r.ini", "memcards", "memcards-mods", "mods", "userdata", "disc-image.txt"], "settings_header": "# ps2r.ini" },
  "settings": { "note": "These are the settings the game shows under Options.", "groups": [
    { "title": "Game", "rows": [
      { "key": "game.text_language", "label": "Text language", "kind": "choice", "default": "english", "write_default": true, "choices_from": "text_languages" },
      { "key": "game.speech_language", "label": "Speech language", "kind": "choice", "default": "english", "write_default": true, "choices_from": "speech_languages" },
      { "key": "game.hz", "label": "Game rate", "kind": "choice", "default": "60", "write_default": true, "choices": [ {"value": "60"}, {"value": "50", "label": "50 (PAL)"} ] } ] },
    { "title": "Picture", "rows": [
      { "key": "video.frame_rate", "label": "Frame rate", "kind": "choice", "default": "60", "write_default": true, "choices": [ {"value": "30"}, {"value": "60"}, {"value": "120"}, {"value": "144"}, {"value": "240"}, {"value": "uncapped", "label": "Uncapped"} ] },
      { "key": "video.vsync", "label": "Vertical sync", "kind": "switch", "default": "off", "write_default": true } ] },
    { "title": "PS Vita", "target": "vita", "rows": [
      { "key": "display.tick_lock", "label": "Frame rate on PS Vita", "kind": "choice", "default": "auto", "choices": [ {"value": "auto", "label": "Auto (60/30)"}, {"value": "60"}, {"value": "30"} ] } ] },
    { "title": "Folders", "rows": [
      { "key": "paths.memcards", "label": "Memory cards", "kind": "folder", "default": "memcards", "write_default": true },
      { "key": "paths.mods", "label": "Mods", "kind": "folder", "default": "mods", "write_default": true } ] },
    { "title": "Disc", "rows": [ { "key": "disc-image.txt", "label": "Disc image", "kind": "disc" } ] } ] },
  "mods": { "builtin": [ { "id": "level_select", "name": "Level select", "author": "TwinsanityEvolved", "switch": false, "help": "A list of every level, boss and area, started at once." },
                         { "id": "speedrun", "name": "Speedrun mode", "author": "TwinsanityEvolved", "help": "A real-time timer at the top of the screen and practice save states (F9 / F10 / F11)." } ] },
  "scan": { "folders": ["~/Downloads", "~/Desktop", "~/Documents"], "removable": false, "max_depth": 2, "min_bytes": 1024 },
  "licences": [ { "label": "MIT licence", "path": "LICENSE" } ]
})JSON";

bool hasEmoji(const QString& s) {
    for (const QChar& c : s) {
        if (c.isHighSurrogate()) return true;
        if (c.unicode() >= 0x2600 && c.unicode() <= 0x27BF) return true;
    }
    return false;
}
// the words a player must never read (the no-codenames lint's list, in short)
const QRegularExpression kDeveloperWords("\\b(cmake|ninja|python|wasm\\w*|wasi\\w*|toolchain|vcpkg|clang\\w*|compiler|compile|nativeee|recomp\\w*|decomp\\w*|vitasdk)\\b",
                                         QRegularExpression::CaseInsensitiveOption);

QByteArray gameZip(const QString& target, const QString& exe) {
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QZipWriter zip(&buf);
    const QString root = "twinsanityevolved-" + target;
    zip.addDirectory(root);
    zip.setCreationPermissions(QFile::Permissions(0x7755));
    // the stand-in game: says it reached the title state, then waits to be stopped
    zip.addFile(root + "/" + exe, "#!/bin/sh\necho '[twinsanityevolved] game state @ boundary 412: Title (7) <- Boot (1), next Title (7)'\nexec sleep 60\n");
    zip.setCreationPermissions(QFile::Permissions(0x6644));
    zip.addFile(root + "/README.md", "The game.\n");
    zip.addFile(root + "/LICENSE", "MIT\n");
    zip.addFile(root + "/BUILD-ID", pattern(400 * 1024, 7));   // some (incompressible) size, so the download can be seen moving
    zip.close();
    return buf.data();
}
}  // namespace

class TestLauncherScreens : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    LauncherApp* app_ = nullptr;
    MainWindow* window_ = nullptr;
    FixtureReleaseServer server_;
    QString out_, realIso_, clone_;
    QStringList shots_;
    QJsonObject report_;   // PS2R_SCREENS_REPORT=FILE: the cards' texts (tools/judges/disc_identity_judge.py reads them)

    void shot(const QString& name) {
        QTest::qWait(250);
        if (out_.isEmpty()) return;
        QDir().mkpath(out_);
        window_->screenshot(out_ + "/launcher-" + name + ".png");
        shots_ << name;
    }
    void visibleTextIsClean() {
        for (QLabel* l : window_->findChildren<QLabel*>()) {
            if (!l->isVisible()) continue;
            const QString t = l->text();
            QVERIFY2(!t.contains("TODO") && !t.contains("Lorem") && !t.contains("placeholder", Qt::CaseInsensitive) && !t.contains("TextLabel"), qPrintable(t));
            QVERIFY2(!hasEmoji(t), qPrintable(t));
            QVERIFY2(!kDeveloperWords.match(t).hasMatch(), qPrintable("developer vocabulary in: " + t));
        }
        for (FlatButton* b : window_->findChildren<FlatButton*>()) {
            if (!b->isVisible()) continue;
            QVERIFY2(!b->text().isEmpty(), "a visible button without text");
            QVERIFY2(!hasEmoji(b->text()), qPrintable(b->text()));
            QVERIFY2(!kDeveloperWords.match(b->text()).hasMatch(), qPrintable(b->text()));
        }
    }

private slots:
    void initTestCase() {
        qputenv("HOME", home_.path().toUtf8());
        qputenv("USERPROFILE", home_.path().toUtf8());
        qputenv("LOCALAPPDATA", home_.path().toUtf8());
        qunsetenv("GITHUB_TOKEN");
        qunsetenv("GH_TOKEN");
        out_ = qEnvironmentVariable("PS2R_SCREENS_OUT");
        realIso_ = qEnvironmentVariable("PS2R_SCREENS_ISO");
        LauncherStyle::apply();
        LauncherTheme::instance().setDark(false);
        // the manifest: the real title's (with its retail lists) or the generated one
        QString manifestPath = qEnvironmentVariable("PS2R_SCREENS_MANIFEST");
        if (manifestPath.isEmpty() || realIso_.isEmpty()) {
            QDir().mkpath(home_.path() + "/titles/discs");
            QFile m(home_.path() + "/titles/title.json");
            QVERIFY(m.open(QIODevice::WriteOnly));
            m.write(kManifest);
            m.close();
            QFile l(home_.path() + "/titles/discs/test.json");
            QVERIFY(l.open(QIODevice::WriteOnly));
            l.write(retailListJson(discFiles()).toUtf8());
            l.close();
            QFile k(home_.path() + "/titles/discs/test-dumps.json");
            QVERIFY(k.open(QIODevice::WriteOnly));
            k.write(dumpTableJson(makeIso(discFiles()), discFiles()[1].second).toUtf8());
            k.close();
            manifestPath = home_.path() + "/titles/title.json";
            realIso_.clear();
        }
        QVERIFY(server_.start());
        const QString host = ReleaseCatalogue::hostTarget();
        const QString exe = host == "windows-x64" ? "twinsanityevolved.exe" : "twinsanityevolved";
        server_.publish("v0.3.0", {{"twinsanityevolved-v0.3.0-" + host + ".zip", gameZip(host, exe)}, {"twinsanityevolved-v0.3.0-vita.vpk", pattern(200000, 9)}});
        // the launcher's settings file names the release server (the one setting a judge needs)
        QDir().mkpath(home_.path() + "/Library/Application Support/TwinsanityEvolved");
        {
            AppData seed("TwinsanityEvolved");
            seed.setValue("releases_url", server_.releasesUrl());
            QVERIFY(seed.save());
        }
        app_ = new LauncherApp(this);
        QString err;
        QVERIFY2(app_->loadManifest(manifestPath, &err), qPrintable(err));
        window_ = new MainWindow(app_);
        window_->resize(1120, 720);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_));
    }
    void cleanupTestCase() {
        if (app_ && app_->game()->running()) app_->game()->stop();
        delete window_;
        if (!clone_.isEmpty()) QFile::remove(clone_);
        if (!out_.isEmpty()) qInfo("screens written: %s", qPrintable(shots_.join(", ")));
        const QString reportPath = qEnvironmentVariable("PS2R_SCREENS_REPORT");
        if (!reportPath.isEmpty()) {
            QFile f(reportPath);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(report_).toJson());
        }
    }

    void setupStartsWithNoDisc() {
        QCOMPARE(window_->minimumSize(), QSize(1024, 640));
        QCOMPARE(window_->sidebar()->pageName(0), QStringLiteral("Games"));
        QCOMPARE(window_->sidebar()->pageName(1), QStringLiteral("Mods"));
        QCOMPARE(window_->sidebar()->pageName(3), QStringLiteral("About"));
        QVERIFY(window_->startSetupIfNeeded());
        SetupSheet* s = window_->setupSheet();
        QVERIFY(s->isOpen());
        QCOMPARE(s->step(), 0);
        QCOMPARE(s->stepLine(), QStringLiteral("Step 1 of 4 · Your game"));
        QVERIFY(!s->backButton()->isEnabled());
        QVERIFY(QTest::qWaitFor([&] { return !s->discStep()->looking(); }, 20000));
        // the looking state (held for its picture: an empty home is searched faster than a frame)
        s->discStep()->showLookingForScreenshot();
        QVERIFY(s->discStep()->looking());
        shot("setup-looking");
        s->discStep()->search();
        QVERIFY(QTest::qWaitFor([&] { return !s->discStep()->looking(); }, 20000));
        QVERIFY(s->discStep()->cards().isEmpty());
        QVERIFY(s->discStep()->emptyState()->isVisible());
        QVERIFY(!s->primaryButton()->isEnabled());
        visibleTextIsClean();
        shot("setup-empty");
    }
    void setupFindsTheDiscs() {
        SetupSheet* s = window_->setupSheet();
        QDir().mkpath(home_.path() + "/Downloads");
        QDir().mkpath(home_.path() + "/Desktop");
        const QString original = home_.path() + "/Downloads/Crash Twinsanity.iso";
        const QString modded = home_.path() + "/Desktop/Crash Twinsanity (mod).iso";
        int expected = 2;
        if (!realIso_.isEmpty()) {
            // the development disc is a MODIFIED image (no known retail dump matches it): one card, "PAL · modified disc"
            QVERIFY(QFile::link(realIso_, original) || QFile::copy(realIso_, original));
            expected = 1;
        } else {
            QFile o(original);
            QVERIFY(o.open(QIODevice::WriteOnly));
            o.write(makeIso(discFiles()));
            o.close();
            QVector<QPair<QString, QByteArray>> files = discFiles();
            files[2].second[100] = char(files[2].second[100] ^ 0x20);
            QFile m(modded);
            QVERIFY(m.open(QIODevice::WriteOnly));
            m.write(makeIso(files));
            m.close();
        }
        s->discStep()->searchButton()->click();
        // the cards appear with the quick identity ("checking…"), then take the whole-image verdict
        QVERIFY(QTest::qWaitFor([&] { return !s->discStep()->cards().isEmpty(); }, 60000));
        report_["first_badge"] = s->discStep()->cards().first()->identityBadgeText();
        report_["first_line"] = s->discStep()->cards().first()->stateLineText();
        QVERIFY(QTest::qWaitFor([&] { return !s->discStep()->looking() && s->discStep()->cards().size() == expected; }, 180000));
        QStringList badges, lines;
        for (DiscChoiceCard* c : s->discStep()->cards()) {
            badges << c->identityBadgeText();
            lines << c->stateLineText();
            QVERIFY2(c->stateBadgeText().isEmpty(), qPrintable(c->stateBadgeText()));   // playable: no second badge
        }
        report_["setup_badges"] = QJsonArray::fromStringList(badges);
        report_["setup_lines"] = QJsonArray::fromStringList(lines);
        report_["disc_verdict"] = s->discStep()->cards().first()->disc().verdict;
        report_["disc_image_sha1"] = s->discStep()->cards().first()->disc().imageSha1;
        report_["disc_image_crc32"] = s->discStep()->cards().first()->disc().imageCrc32;
        if (realIso_.isEmpty()) {
            QVERIFY2(badges.contains("PAL · original disc"), qPrintable(badges.join(", ")));
            QVERIFY2(badges.contains("PAL · modified disc"), qPrintable(badges.join(", ")));
            QCOMPARE(s->discStep()->chosen().size(), 1);   // the original is chosen for the player
            QCOMPARE(s->discStep()->chosen().first().path, QFileInfo(original).absoluteFilePath());
            for (DiscChoiceCard* c : s->discStep()->cards())
                if (c->disc().path != QFileInfo(original).absoluteFilePath())
                    QCOMPARE(c->stateLineText(), QStringLiteral("A modified copy of the PAL disc: the size of the original, with other contents."));
        } else {
            QCOMPARE(badges, QStringList{"PAL · modified disc"});
            QCOMPARE(s->discStep()->cards().first()->stateLineText(), QStringLiteral("A modified copy of the PAL disc: 20 MB larger than the original."));
            QVERIFY(s->discStep()->chosen().isEmpty());   // only an original disc is chosen for the player
            QTest::mouseClick(s->discStep()->cards().first(), Qt::LeftButton);
            QCOMPARE(s->discStep()->chosen().size(), 1);
        }
        QVERIFY(s->primaryButton()->isEnabled());
        visibleTextIsClean();
        shot("setup-game");
        // a file that is not a disc image: the banner says so, the cards stay
        QFile junk(home_.path() + "/Downloads/notes.iso");
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(QByteArray(64 * 1024, 'x'));
        junk.close();
        s->discStep()->addFromPath(junk.fileName());
        QVERIFY(QTest::qWaitFor([&] { return !s->discStep()->bannerText().isEmpty(); }, 20000));
        QVERIFY(s->discStep()->bannerText().startsWith("notes.iso cannot be used. It is not a disc image"));
        QCOMPARE(s->discStep()->cards().size(), realIso_.isEmpty() ? 2 : 1);
        shot("setup-error");
        QFile::remove(junk.fileName());
    }
    void setupFolder() {
        SetupSheet* s = window_->setupSheet();
        s->primaryButton()->click();
        QCOMPARE(s->step(), 1);
        QVERIFY(s->backButton()->isEnabled());
        QCOMPARE(s->folderStep()->folder(), home_.path() + "/Games/Crash Twinsanity");
        QVERIFY(s->folderStep()->valid());
        visibleTextIsClean();
        shot("setup-folder");
        // a folder that cannot be written (a read-only folder: POSIX permissions; Windows folders ignore the read-only bit)
#ifndef Q_OS_WIN
        const QString locked = home_.path() + "/Locked";
        QDir().mkpath(locked);
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::ExeOwner);
        s->folderStep()->setFolder(locked + "/Crash Twinsanity", true);
        QVERIFY(!s->folderStep()->valid());
        QCOMPARE(s->folderStep()->problem(), QStringLiteral("This folder cannot be written to. Choose another one."));
        QVERIFY(!s->primaryButton()->isEnabled());
        shot("setup-folder-error");
        QFile::setPermissions(locked, QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner);
#endif
        s->folderStep()->setFolder(home_.path() + "/Games/Crash Twinsanity", true);
        QVERIFY(s->folderStep()->valid());
        // Back works, and Next again
        s->backButton()->click();
        QCOMPARE(s->step(), 0);
        s->primaryButton()->click();
        QCOMPARE(s->step(), 1);
    }
    void setupWhereToPlay() {
        SetupSheet* s = window_->setupSheet();
        s->primaryButton()->click();
        QCOMPARE(s->step(), 2);
        QCOMPARE(s->primaryButton()->text(), QStringLiteral("Install"));
        QVERIFY(s->targetStep()->thisComputer()->chosen());
        QCOMPARE(s->targetStep()->target(), app_->hostTarget());
        visibleTextIsClean();
        shot("setup-where");
        QTest::mouseClick(s->targetStep()->vita(), Qt::LeftButton);
        QCOMPARE(s->targetStep()->target(), QStringLiteral("vita"));
        QVERIFY(s->targetStep()->vitaNote().contains("ur0:data/libshacccg.suprx"));
        QVERIFY(s->targetStep()->vitaNote().contains("ux0:data/twinsanity/" + app_->vitaConsoleDisc()));
        QVERIFY(s->targetStep()->vitaNote().contains("so that it is ux0:data/twinsanity there"));
        QVERIFY(s->targetStep()->vitaNote().contains("VitaShell"));
        QVERIFY(!s->targetStep()->vitaNote().contains("{"));
        // which disc the console starts (disc-image.txt), the North American disc beside the European one
        QVERIFY(s->targetStep()->vitaNote().contains("disc-image.txt names the disc the game starts"));
        QVERIFY(s->targetStep()->vitaNote().contains("game.iso and ntsc.iso"));
        QTest::qWait(100);
        QLabel* note = s->targetStep()->findChild<QLabel*>(QStringLiteral("target_vita_note"));
        QVERIFY(note && note->isVisible());
        QVERIFY2(note->height() >= note->heightForWidth(note->width()),
                 qPrintable(QStringLiteral("%1 < %2").arg(note->height()).arg(note->heightForWidth(note->width()))));
        shot("setup-where-vita");
        QTest::mouseClick(s->targetStep()->another(), Qt::LeftButton);
        QVERIFY(s->targetStep()->target() != app_->hostTarget() && s->targetStep()->target() != "vita");
        QVERIFY(!s->targetStep()->otherPlatforms()->values().contains(app_->hostTarget()));
        shot("setup-where-other");
        QTest::mouseClick(s->targetStep()->thisComputer(), Qt::LeftButton);
        QCOMPARE(s->targetStep()->target(), app_->hostTarget());
    }
    void setupInstallFailsOffline() {
        SetupSheet* s = window_->setupSheet();
        server_.stop();
        s->primaryButton()->click();
        QCOMPARE(s->step(), 3);
        QVERIFY(QTest::qWaitFor([&] { return s->installStep()->failed(); }, 40000));
        QCOMPARE(s->installStep()->errorText(), QStringLiteral("The download did not finish. Check your connection and try again."));
        QCOMPARE(s->primaryButton()->text(), QStringLiteral("Try again"));
        QVERIFY(s->backButton()->isEnabled());
        visibleTextIsClean();
        shot("setup-install-error");
    }
    void setupInstalls() {
        SetupSheet* s = window_->setupSheet();
        QVERIFY(server_.start());   // the same port again
        server_.setRateLimit(160 * 1024);
        s->primaryButton()->click();   // Try again
        QVERIFY(QTest::qWaitFor([&] { return app_->installer()->phase() == ReleaseInstaller::Downloading && app_->installer()->received() > 100000; }, 30000));
        QVERIFY(!s->primaryButton()->isEnabled());
        QVERIFY(s->installStep()->progress()->caption().contains(" of "));
        visibleTextIsClean();
        shot("setup-installing");
        QVERIFY(QTest::qWaitFor([&] { return s->installStep()->done() || s->installStep()->failed(); }, 60000));
        QVERIFY2(s->installStep()->done(), qPrintable(s->installStep()->errorText()));
        server_.setRateLimit(0);
        QCOMPARE(s->primaryButton()->text(), QStringLiteral("Play"));
        QCOMPARE(s->installStep()->statusText(), QStringLiteral("Crash Twinsanity is ready."));
        const QString folder = home_.path() + "/Games/Crash Twinsanity";
        QVERIFY(QFileInfo(folder + "/twinsanityevolved").isExecutable() || QFileInfo(folder + "/twinsanityevolved.exe").exists());
        QVERIFY(QFileInfo(folder + "/ps2r.ini").isFile());
        QVERIFY(QFileInfo(folder + "/memcards").isDir());
        QVERIFY(QFileInfo(folder + "/mods").isDir());
        QVERIFY(QFileInfo(folder + "/disc-image.txt").isFile());
        QVERIFY(app_->setupDone());
        visibleTextIsClean();
        shot("setup-done");
    }
    void playFromTheDoneScreen() {
        SetupSheet* s = window_->setupSheet();
#ifdef Q_OS_WIN
        s->close();   // the stand-in game is a shell script: it is played on macOS and Linux
        QSKIP("the stand-in game is a shell script");
#endif
        s->primaryButton()->click();   // Play
        QVERIFY(!s->isOpen());
        QVERIFY(QTest::qWaitFor([&] { return app_->game()->running(); }, 10000));
        QVERIFY(QTest::qWaitFor([&] { return app_->game()->reachedTitle(); }, 20000));
        const QStringList args = app_->game()->lastArguments();
        QVERIFY(args.contains("--config"));
        QVERIFY(args.contains("--trace-state"));
        QVERIFY(!args.contains("--mc-dir"));   // ps2r.ini says where the memory cards are
        QCOMPARE(window_->currentPage(), int(MainWindow::GamesIndex));
        shot("games-playing");
        app_->stopGame();
        QVERIFY(QTest::qWaitFor([&] { return !app_->game()->running(); }, 15000));
    }
    void gamesReadyAndUpdate() {
        GamesPage* g = window_->gamesPage();
        QCOMPARE(g->cards().size(), 1);
        GameCard* c = g->cards().first();
        QCOMPARE(c->actionButton()->text(), QStringLiteral("Play"));
        report_["games_badge"] = c->identityBadgeText();
        report_["games_state_badge"] = c->stateBadgeText();
        report_["games_status"] = c->statusText();
        QCOMPARE(c->identityBadgeText(), realIso_.isEmpty() ? QStringLiteral("PAL · original disc") : QStringLiteral("PAL · modified disc"));
        QVERIFY(c->stateBadgeText().isEmpty());
        if (realIso_.isEmpty()) QCOMPARE(c->statusText(), QStringLiteral("Ready to play."));
        else QCOMPARE(c->statusText(), QStringLiteral("Ready to play. A modified copy of the PAL disc: 20 MB larger than the original."));
        visibleTextIsClean();
        shot("games");
        // a newer release: Update on the card
        const QString host = ReleaseCatalogue::hostTarget();
        server_.publish("v0.4.0", {{"twinsanityevolved-v0.4.0-" + host + ".zip", gameZip(host, host == "windows-x64" ? "twinsanityevolved.exe" : "twinsanityevolved")},
                                   {"twinsanityevolved-v0.4.0-vita.vpk", pattern(200000, 10)}});
        QSignalSpy upd(app_, &LauncherApp::updateAvailable);
        app_->checkForUpdates();
        QVERIFY(upd.wait(10000));
        QVERIFY(QTest::qWaitFor([&] { return c->updateButton()->isVisible(); }, 5000));
        QCOMPARE(c->updateButton()->text(), QStringLiteral("Update to v0.4.0"));
        QVERIFY(c->statusText().contains("v0.4.0"));
        shot("games-update");
        c->updateButton()->click();
        QVERIFY(QTest::qWaitFor([&] { return !g->updateText().isEmpty(); }, 30000));
        QVERIFY2(g->updateText().startsWith("Updated to v0.4.0."), qPrintable(g->updateText()));
        QCOMPARE(app_->playableInstall().tag, QStringLiteral("v0.4.0"));
        QVERIFY(QFileInfo(home_.path() + "/Games/Crash Twinsanity/ps2r.ini").isFile());   // kept
    }
    // The launcher's own update: a newer launcher in the list is offered at the top of the Games page. The test's
    // program is no launcher installed from a release (it would only give the release's page), so a stand-in install in
    // the test's home is the running launcher: the offer as a player's installed launcher shows it. Not now hides it here;
    // About keeps it.
    void gamesLauncherUpdate() {
        window_->showPage(MainWindow::GamesIndex);
        const QString host = ReleaseCatalogue::hostTarget();
        const QString file = app_->manifest()->launcherAssetPattern(host);
        QVERIFY2(!file.isEmpty(), "the manifest names no launcher file for this computer");
        server_.addRelease(QStringLiteral("v0.4.1"), {{ReleaseCatalogue::assetName(file, QStringLiteral("v0.4.1"), host), pattern(300000, 11)}});
        RunningLauncher r = SelfUpdate::detect();
        r.version = QStringLiteral("v0.4.0");   // the launcher of the release the game came from
        r.made = QDateTime::currentDateTimeUtc().addDays(-1);
#if defined(Q_OS_MACOS)
        r.kind = QStringLiteral("app");
        r.path = home_.path() + "/Applications/TwinsanityEvolved.app";
        r.program = QStringLiteral("ps2r_launcher");
        QVERIFY(QDir().mkpath(r.path + "/Contents/MacOS"));
#elif defined(Q_OS_WIN)
        r.kind = QStringLiteral("folder");
        r.path = home_.path() + "/TwinsanityEvolved Launcher";
        r.program = QStringLiteral("TwinsanityEvolved.exe");
        QVERIFY(QDir().mkpath(r.path));
#else
        r.kind = QStringLiteral("appimage");
        r.path = home_.path() + "/Applications/TwinsanityEvolved.AppImage";
        r.program = QFileInfo(r.path).fileName();
        QVERIFY(QDir().mkpath(QFileInfo(r.path).absolutePath()));
        QFile stand(r.path);
        QVERIFY(stand.open(QIODevice::WriteOnly));
        stand.write("stand-in");
        stand.close();
#endif
        SelfUpdate* su = app_->selfUpdate();
        su->setRunning(r);
        QVERIFY(QTest::qWaitFor([&] { return su->state() != SelfUpdate::Checking; }, 20000));   // the check made at start
        su->check();
        QVERIFY(QTest::qWaitFor([&] { return su->state() != SelfUpdate::Checking; }, 20000));
        QCOMPARE(int(su->state()), int(SelfUpdate::Available));
        QCOMPARE(su->release().tag, QStringLiteral("v0.4.1"));
        QCOMPARE(su->cannotReplace(), SelfUpdate::NoProblem);
        LauncherUpdateBox* box = window_->gamesPage()->launcherUpdateBox();
        QVERIFY(QTest::qWaitFor([&] { return box->isVisible(); }, 3000));
        QCOMPARE(box->text(), QStringLiteral("A new launcher is ready: v0.4.1."));
        QVERIFY(box->updateButton()->isVisible() && box->updateButton()->isEnabled());
        QCOMPARE(box->updateButton()->text(), QStringLiteral("Update the launcher to v0.4.1"));
        QVERIFY(box->laterButton()->isVisible());
        QVERIFY(!box->downloadButton()->isVisible());
        QVERIFY(app_->updateFor(app_->hostTarget()).isEmpty());   // the launcher's release offers no game update
        visibleTextIsClean();
        shot("games-launcher-update");
        box->laterButton()->click();   // Not now
        QVERIFY(QTest::qWaitFor([&] { return !box->isVisible(); }, 3000));
        QCOMPARE(int(su->state()), int(SelfUpdate::Available));
    }
    void modsPage() {
        window_->showPage(MainWindow::ModsIndex);
        ModsPage* m = window_->modsPage();
        QVERIFY(m->emptyState()->isVisible());
        QVERIFY(m->row("level_select"));
        visibleTextIsClean();
        shot("mods-empty");
        // a mod folder dropped on the page is copied in as it is
        const QString modDir = home_.path() + "/Downloads/hubtest";
        QDir().mkpath(modDir + "/disc/CRASH6");
        QFile j(modDir + "/mod.json");
        QVERIFY(j.open(QIODevice::WriteOnly));
        j.write(R"J({"schema": 2, "id": "hubtest", "kind": "disc", "name": "Hub test", "author": "A modder", "version": "1.2",
                     "description": "Changes the hub. Nothing else.", "base": {"region": "PAL", "executable": "SLES_525.68"},
                     "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges"}]})J");
        j.close();
        m->dropPath(modDir);
        QVERIFY2(m->noticeText().startsWith("Hub test was added."), qPrintable(m->noticeText()));
        QVERIFY(m->row("hubtest"));
        QVERIFY(!m->emptyState()->isVisible());
        m->row("hubtest")->toggle()->click();
        QCOMPARE(app_->settingValue("mods.hubtest.enabled"), QStringLiteral("on"));
        // one more, as the import writes it: a modified executable (data and code changes); and the release's Arabic language
        // pack beside them, which is not a mod: in the folder, on no row
        const QString mods = app_->modsFolder();
        QDir().mkpath(mods + "/agony/disc");
        QFile a(mods + "/agony/mod.json");
        QVERIFY(a.open(QIODevice::WriteOnly));
        a.write(R"J({"schema": 2, "id": "agony", "kind": "disc", "name": "Ant agony remix", "author": "Someone", "version": "1.2",
                     "base": {"region": "PAL", "executable": "SLES_525.68"},
                     "files": [{"path": "SLES_525.68", "form": "replace"}, {"path": "CRASH6/CRASH.BH", "form": "replace"}, {"path": "CRASH6/CRASH.BD", "form": "ranges"}],
                     "executable": {"data_changes": {".rodata": 4}, "code_changes": ["FUN_001ac8d0 Font::ConstructBase"]}})J");
        a.close();
        QDir().mkpath(mods + "/arabic/data");
        QFile l(mods + "/arabic/mod.json");
        QVERIFY(l.open(QIODevice::WriteOnly));
        l.write(R"J({"schema": 2, "id": "arabic", "kind": "language", "name": "arabic", "author": "TwinsanityEvolved", "version": "1.1.0",
                     "base": {"region": "PAL", "executable": "SLES_525.68"}, "files": [],
                     "language": {"id": "arabic", "label": "arabic", "text_bank": "data/arabic.bank", "direction": "rtl"}})J");
        l.close();
        m->reload();
        QCOMPARE(m->folderOrder(), QStringList({"agony", "hubtest"}));
        QVERIFY(!m->row("arabic"));
        QCOMPARE(m->row("agony")->warningText(), QStringLiteral("Code changes not supported: FUN_001ac8d0 Font::ConstructBase."));
        QVERIFY(m->moveMod("hubtest", -1));   // the order: hubtest above agony
        QCOMPARE(app_->settingValue("mods.order"), QStringLiteral("hubtest,agony"));
        visibleTextIsClean();
        shot("mods");
        // importing (held for its picture: a real import takes a while), then a refused artefact
        m->showImportingForScreenshot("Cave Remix v2.0.zip", "reading the mod crate", 0.4);
        QVERIFY(m->progress()->isVisible());
        visibleTextIsClean();
        shot("mods-importing");
        m->showImportingForScreenshot(QString(), QString(), 0.0);
#ifdef PS2R_LAUNCHER_IMPORT_library
        // a crate the import library refuses: Crate Mod Loader's property edits are operations of the loader's own code
        const QString crate = home_.path() + "/Downloads/Speedy Crash";
        QDir().mkpath(crate + "/layer0");
        QFile info(crate + "/ModCrateInfo.txt");
        QVERIFY(info.open(QIODevice::WriteOnly));
        info.write("Name=Speedy Crash\nAuthor=A modder\nVersion=1.0\n");
        info.close();
        QFile settings(crate + "/ModCrateSettings.txt");
        QVERIFY(settings.open(QIODevice::WriteOnly));
        settings.write("CrashSpeed=2\n");
        settings.close();
        m->dropPath(crate);
        QVERIFY(QTest::qWaitFor([&] { return !m->importing(); }, 300000));
        QVERIFY2(m->noticeIsError() && m->noticeText().startsWith("Speedy Crash could not be added. Speedy Crash changes the game's code settings"),
                 qPrintable(m->noticeText()));
#else
        m->dropPath(home_.path() + "/Downloads/Crash Twinsanity.iso");
        QVERIFY(QTest::qWaitFor([&] { return !m->importing(); }, 120000));
        QCOMPARE(m->noticeText(), QStringLiteral("Crash Twinsanity.iso could not be added. Importing is not available in this version of the launcher."));
#endif
        visibleTextIsClean();
        shot("mods-refused");
        // Remove asks first
        m->row("agony")->removeButton()->click();
        QVERIFY(m->removeSheet() && m->removeSheet()->isVisible());
        visibleTextIsClean();
        shot("mods-remove");
        m->removeSheet()->findChild<FlatButton*>("mods_remove_cancel")->click();
        QVERIFY(m->row("agony"));
        // the same mod folder again: one rule, replace -- asked first
        m->dropPath(modDir);
        QVERIFY(m->replaceSheet() && m->replaceSheet()->isVisible());
        visibleTextIsClean();
        shot("mods-replace");
        m->replaceSheet()->findChild<FlatButton*>("mods_replace_cancel")->click();
        QCOMPARE(m->noticeText(), QStringLiteral("Nothing was added: you keep Hub test 1.2."));
        // a mod made over the North American disc, on this European game folder: the badge, the region in words, "Left out on
        // this disc" (its archive recipe records the North American archive, as an import writes it: the game's base check
        // refuses it here), the Crate Mod Loader settings it asks for and its notes
        QDir().mkpath(mods + "/nitro_challenge/disc/CRASH6");
        QFile u(mods + "/nitro_challenge/mod.json");
        QVERIFY(u.open(QIODevice::WriteOnly));
        u.write(R"J({"schema": 2, "id": "nitro_challenge", "kind": "disc", "name": "Nitro Challenge", "author": "A modder", "version": "v1.6",
                     "notes": ["For the mod to work, apply Classic Slide Jump for Crash and Classic Explosion Damage.", "Good luck."],
                     "base": {"region": "NTSC-U", "executable": "SLUS_209.09"},
                     "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges", "base_bytes": 890851747}, {"path": "CRASH6/CRASH.BH", "form": "replace"},
                               {"path": "CRASH6/MUSIC.MB", "form": "replace"}, {"path": "CRASH6/MUSIC.MH", "form": "replace"}],
                     "import": {"game_code_settings": ["Classic Slide Jump for Crash", "Classic Explosion Damage"]}})J");
        u.close();
        m->reload();
        QVERIFY(m->row("nitro_challenge"));
        QCOMPARE(m->row("nitro_challenge")->regionText(), QStringLiteral("NTSC-U"));
        if (app_->importBaseRegion() == QLatin1String("PAL")) {
            QCOMPARE(m->row("nitro_challenge")->summaryText(), QStringLiteral("Changes 4 files, made for the North American disc."));
            QCOMPARE(m->row("nitro_challenge")->warningText(),
                     QStringLiteral("Left out on this disc: made for the North American disc.\nIt asks for Crate Mod Loader settings the game cannot "
                                    "apply: Classic Slide Jump for Crash, Classic Explosion Damage."));
        }
        if (QScrollArea* scroll = m->findChild<QScrollArea*>()) {   // the row is at the end of the list: the page's end
            QTest::qWait(50);
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QTest::qWait(50);
        }
        visibleTextIsClean();
        shot("mods-region");
        // the PS Vita panel at the foot: before a PS Vita install it leads to one
        QVERIFY(m->vitaPanel() && m->vitaButton());
        QVERIFY2(m->vitaPanelText().startsWith("On the PS Vita, mods are switched here and copied"), qPrintable(m->vitaPanelText()));
        QVERIFY(m->vitaPanelText().contains("ux0/data/twinsanity") && !m->vitaPanelText().contains("{"));
        QCOMPARE(m->vitaButton()->text(), QStringLiteral("Get the game for PS Vita…"));
    }
    void settingsPage() {
        window_->showPage(MainWindow::SettingsIndex);
        SettingsPage* s = window_->settingsPage();
        QVERIFY(!s->emptyState()->isVisible());
        QVERIFY(s->row("video.frame_rate"));
        QVERIFY(s->folderRow("paths.memcards"));
        QCOMPARE(s->folderRow("paths.memcards")->path(), home_.path() + "/Games/Crash Twinsanity/memcards");
        // the two language rows: what the game offers for the disc it starts with -- with the development disc its text
        // languages by content (and, with PS2R_SCREENS_PACKS, the release's Arabic pack placed as the install places it) and
        // its speech banks; without a disc of the game, english alone
        SettingsRow* text = s->row("game.text_language");
        SettingsRow* speech = s->row("game.speech_language");
        QVERIFY(text && speech);
        auto values = [](const SettingsRow* r) {
            QStringList v;
            for (const SettingsChoice& c : r->choices()) v << c.value;
            return v;
        };
        if (!realIso_.isEmpty()) {
            const QString packs = qEnvironmentVariable("PS2R_SCREENS_PACKS");
            QStringList wantText = {"english", "portuguese", "french", "german", "italian"};
            if (!packs.isEmpty() && QFileInfo(packs + "/arabic/mod.json").isFile()) {
                const QString dest = app_->modsFolder() + "/arabic";
                QDir(dest).removeRecursively();
                QDirIterator it(packs + "/arabic", QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString from = it.next();
                    const QString to = dest + from.mid((packs + "/arabic").size());
                    QDir().mkpath(QFileInfo(to).absolutePath());
                    QVERIFY(QFile::copy(from, to));
                }
                wantText << "arabic";
                s->reload();
            }
            QCOMPARE(values(text), wantText);
            QCOMPARE(values(speech), QStringList({"english", "french", "german", "spanish", "italian"}));
        } else {
            QCOMPARE(values(text), QStringList({"english"}));
            QCOMPARE(values(speech), QStringList({"english"}));
        }
        QCOMPARE(text->value(), QStringLiteral("english"));
        visibleTextIsClean();
        shot("settings");
    }
    void aboutPage() {
        window_->showPage(MainWindow::AboutIndex);
        AboutPage* about = window_->aboutPage();
        QVERIFY(about->removeButton());
        QVERIFY(about->versionsText().contains("Game v0.4.0"));
        // the launcher's own update stays offered here after Not now on Games
        QCOMPARE(about->launcherUpdateBox()->text(), QStringLiteral("A new launcher is ready: v0.4.1."));
        QVERIFY(about->launcherUpdateBox()->updateButton()->isVisible());
        visibleTextIsClean();
        shot("about");
        // the PS Vita in six sentences, the names filled in
        const QString vita = about->vitaText();
        QCOMPARE(vita.split('\n', Qt::SkipEmptyParts).size(), 6);
        QVERIFY2(vita.contains("VitaShell") && vita.contains("ntsc.iso") && vita.contains("ux0:data/twinsanity/memcards") && !vita.contains("{"),
                 qPrintable(vita));
        // shown whole: the label is as tall as its text at its width (a word-wrapped label given less loses its last line)
        QLabel* label = about->findChild<QLabel*>(QStringLiteral("about_vita"));
        QVERIFY(label && label->isVisible());
        QVERIFY2(label->height() >= label->heightForWidth(label->width()),
                 qPrintable(QStringLiteral("%1 < %2").arg(label->height()).arg(label->heightForWidth(label->width()))));
        window_->detailsSheet()->showLog("Details", "line one\nline two");
        QVERIFY(window_->detailsSheet()->isOpen());
        window_->detailsSheet()->closeSheet();
    }
    void anotherPlace() {
        window_->showPage(MainWindow::GamesIndex);
        window_->gamesPage()->anotherPlaceButton()->click();
        SetupSheet* s = window_->setupSheet();
        QVERIFY(s->isOpen());
        QCOMPARE(s->step(), 1);
        QCOMPARE(s->backButton()->text(), QStringLiteral("Cancel"));
        // the PS Vita folder is made from the game folder, inside it
        const QString game = app_->gameFolder();
        const QString vita = game + "/vita";
        QCOMPARE(s->folderStep()->folder(), vita);
        s->primaryButton()->click();
        QCOMPARE(s->targetStep()->target(), QStringLiteral("vita"));
        s->primaryButton()->click();   // Install
        QVERIFY(QTest::qWaitFor([&] { return s->installStep()->done() || s->installStep()->failed(); }, 30000));
        QVERIFY2(s->installStep()->done(), qPrintable(s->installStep()->errorText()));
        QCOMPARE(s->primaryButton()->text(), QStringLiteral("Done"));
        QCOMPARE(s->installStep()->errorText(), QString());
        QVERIFY(QFileInfo(vita + "/twinsanityevolved.vpk").isFile());
        // ready to copy: the README, the console's data folder with the game folder's settings (the computer's own keys
        // left out), the language packs it has, the disc's name; never the disc image, never the saves
        QVERIFY(QFileInfo(vita + "/README-vita.txt").isFile());
        {
            // the README: the disc under disc-image.txt's name, the two discs side by side, mods made again
            QFile readme(vita + "/README-vita.txt");
            QVERIFY(readme.open(QIODevice::ReadOnly));
            const QString text = QString::fromUtf8(readme.readAll());
            QVERIFY2(text.contains("disc-image.txt names the disc the game starts") && text.contains("game.iso and ntsc.iso") &&
                         text.contains("The North American disc runs at 60 Hz and keeps its own memory card.") &&
                         text.contains("(on its Mods or Settings page)") && !text.contains("{"),
                     qPrintable(text));
        }
        const QString data = vita + "/ux0/data/twinsanity";
        IniSettings copied(data + "/ps2r.ini");
        QVERIFY(copied.exists());
        QVERIFY(!copied.has("game.hz") && !copied.has("paths.mods") && !copied.has("paths.memcards") && !copied.has("video.frame_rate"));
        QCOMPARE(copied.get("game.text_language"), IniSettings(game + "/ps2r.ini").get("game.text_language"));
        QFile discName(data + "/disc-image.txt");
        QVERIFY(discName.open(QIODevice::ReadOnly));
        QCOMPARE(QString::fromUtf8(discName.readAll()).trimmed(), app_->vitaConsoleDisc());
        QVERIFY(!QFileInfo::exists(data + "/memcards"));
        QCOMPARE(app_->lastVitaFolder().folder, vita);
        QCOMPARE(app_->gameFolder(), game);   // the PS Vita install is not a game folder: the settings stay the computer's
        visibleTextIsClean();
        shot("setup-done-vita");
        s->primaryButton()->click();
        QVERIFY(!s->isOpen());
        QCOMPARE(app_->installs().size(), 2);
        QCOMPARE(app_->vitaFolder(), vita);
        // Settings > PS Vita: made again after a change (a setting the console keeps, one it leaves out)
        window_->showPage(MainWindow::SettingsIndex);
        SettingsPage* page = window_->settingsPage();
        page->reload();
        QVERIFY(page->vitaFolderButton() && page->vitaFolderButton()->isVisible());
        QVERIFY(app_->setSettingValue("display.tick_lock", "30"));
        QVERIFY(app_->setSettingValue("game.hz", "50"));
        page->reload();   // the rows read the file again
        page->vitaFolderButton()->click();
        QVERIFY2(page->vitaFolderLine().startsWith("Made again in "), qPrintable(page->vitaFolderLine()));
        QCOMPARE(IniSettings(data + "/ps2r.ini").get("display.tick_lock"), QStringLiteral("30"));
        QVERIFY(!IniSettings(data + "/ps2r.ini").has("game.hz"));
        QCoreApplication::processEvents();
        if (auto* scroll = page->findChild<QScrollArea*>())
            if (auto* line = page->findChild<QLabel*>(QStringLiteral("settings_vita_folder_line"))) scroll->ensureWidgetVisible(line, 0, 160);
        visibleTextIsClean();
        shot("settings-vita-folder");
        // the Mods page's PS Vita panel: the same button, right where the mods are switched
        window_->showPage(MainWindow::ModsIndex);
        ModsPage* mods = window_->modsPage();
        mods->reload();
        QCOMPARE(mods->vitaButton()->text(), QStringLiteral("Make the PS Vita folder again"));
        mods->vitaButton()->click();
        QVERIFY2(mods->vitaLineText().startsWith("Made again in "), qPrintable(mods->vitaLineText()));
        QCoreApplication::processEvents();
        if (auto* scroll = mods->findChild<QScrollArea*>()) {
            QTest::qWait(50);
            scroll->verticalScrollBar()->setValue(scroll->verticalScrollBar()->maximum());
            QTest::qWait(50);
        }
        visibleTextIsClean();
        shot("mods-vita");
        window_->showPage(MainWindow::GamesIndex);
    }
    // the North American disc beside the European one (PS2R_SCREENS_ISO2: a retail NTSC-U image, with PS2R_SCREENS_ISO the
    // user's modified PAL image): two cards side by side, each with its identity once its image is hashed -- "NTSC-U ·
    // original disc" (playable: the title plays SLUS_209.09) and "PAL · modified disc". Skipped without the two discs.
    void gamesTwoDiscs() {
        const QString second = qEnvironmentVariable("PS2R_SCREENS_ISO2");
        if (realIso_.isEmpty() || second.isEmpty()) QSKIP("PS2R_SCREENS_ISO / PS2R_SCREENS_ISO2 not set");
        window_->showPage(MainWindow::GamesIndex);
        GamesPage* g = window_->gamesPage();
        const QString linked = home_.path() + "/Downloads/Crash Twinsanity (USA).iso";
        QVERIFY(QFile::link(second, linked) || QFile::copy(second, linked));
        g->addDiscFromPath(linked);
        auto card = [&](const QString& badge) -> GameCard* {
            for (GameCard* c : g->cards())
                if (c->identityBadgeText() == badge) return c;
            return nullptr;
        };
        QVERIFY(QTest::qWaitFor([&] { return g->cards().size() == 2; }, 60000));
        QVERIFY(QTest::qWaitFor([&] { return card(QStringLiteral("NTSC-U · original disc")) != nullptr; }, 600000));
        GameCard* usa = card(QStringLiteral("NTSC-U · original disc"));
        GameCard* pal = card(QStringLiteral("PAL · modified disc"));
        QVERIFY(usa && pal);
        report_["two_discs_usa_badge"] = usa->identityBadgeText();
        report_["two_discs_usa_status"] = usa->statusText();
        report_["two_discs_usa_state_badge"] = usa->stateBadgeText();
        report_["two_discs_pal_badge"] = pal->identityBadgeText();
        QVERIFY(usa->stateBadgeText().isEmpty());   // no "Not playable in this version": the title plays SLUS_209.09
        QCOMPARE(usa->statusText(), QStringLiteral("Ready to play."));
        QCOMPARE(usa->actionButton()->text(), QStringLiteral("Play"));
        visibleTextIsClean();
        shot("games-two-discs");
    }
    void gamesEmptyAndRefused() {
        GamesPage* g = window_->gamesPage();
        while (!app_->library()->games().isEmpty()) app_->library()->remove(app_->library()->games().first().disc.path);
        QVERIFY(QTest::qWaitFor([&] { return g->emptyState()->isVisible(); }, 3000));
        shot("games-empty");
        QFile junk(home_.path() + "/Downloads/other.iso");
        QVERIFY(junk.open(QIODevice::WriteOnly));
        junk.write(makeIso({{"README.TXT", "hello"}}));
        junk.close();
        g->addDiscFromPath(junk.fileName());
        bool shown = false;
        QVERIFY(QTest::qWaitFor([&] {
            for (QLabel* l : g->emptyState()->findChildren<QLabel*>()) shown |= l->text().contains("not of a PlayStation 2 game");
            return shown;
        }, 20000));
        visibleTextIsClean();
    }
    void darkFollowsTheme() {
        LauncherTheme::instance().setDark(true);
        QVERIFY(LauncherTheme::instance().dark());
        QCOMPARE(LauncherTheme::instance().color(LauncherTheme::Accent).name(), QStringLiteral("#f0693a"));
        LauncherTheme::instance().setDark(false);
        QCOMPARE(LauncherTheme::instance().color(LauncherTheme::Accent).name(), QStringLiteral("#d9541e"));
    }
};

QTEST_MAIN(TestLauncherScreens)
#include "test_launcher_screens.moc"
