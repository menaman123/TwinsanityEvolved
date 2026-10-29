// tests/app/launcher/test_launcher_cold.cpp -- the cold run of the prebuilt-install flow, driven through the window's own
// buttons (the cold judge's driver): with HOME at a folder holding only the player's disc image (in Downloads) and the
// launcher's settings file naming the release server, the setup opens by itself; step 1 finds and identifies the disc,
// Next; step 2 keeps the suggested game folder, Next; step 3 keeps This computer, Install; the release is downloaded,
// checked and placed; Play; the game reaches its title state in the launcher's own play log. Zero commands typed.
// Then the launcher's own update (launcherUpdate): the list also names a NEWER LAUNCHER; the check made when the window
// opened offers it on the Games page and on About, and -- this program being no launcher installed from a release -- the
// offer says it cannot replace itself here and gives the release's page; the game's own Update is not offered for it.
// Writes a JSON report (wall time, bytes downloaded, the steps' timings, the game folder as installed, the offer).
//
// Environment: PS2R_COLD_MANIFEST (title.json), PS2R_COLD_REPORT, PS2R_COLD_SCREENSHOTS (one PNG per step),
// PS2R_COLD_INSTALL_TIMEOUT_S (default 600), PS2R_COLD_TITLE_TIMEOUT_S (default 300).
#include <QtTest/QtTest>
#include <QDirIterator>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "app_data.h"
#include "choice_card.h"
#include "disc_choice_card.h"
#include "flat_button.h"
#include "game_library.h"
#include "game_process.h"
#include "about_page.h"
#include "games_page.h"
#include "launcher_app.h"
#include "launcher_style.h"
#include "launcher_update_box.h"
#include "main_window.h"
#include "release_installer.h"
#include "self_update.h"
#include "setup_disc_step.h"
#include "setup_folder_step.h"
#include "setup_install_step.h"
#include "setup_sheet.h"
#include "setup_target_step.h"
using namespace ps2r::launcher;

class TestLauncherCold : public QObject {
    Q_OBJECT
    LauncherApp* app_ = nullptr;
    MainWindow* window_ = nullptr;
    QJsonObject report_;
    QElapsedTimer wall_;
    QString shots_;
    QStringList clicks_;

    int envInt(const char* name, int fallback) {
        const QByteArray v = qgetenv(name);
        return v.isEmpty() ? fallback : v.toInt();
    }
    void shot(const QString& name) {
        if (shots_.isEmpty()) return;
        QDir().mkpath(shots_);
        window_->screenshot(shots_ + "/" + name + ".png");
    }
    void press(FlatButton* b, const QString& what) {
        QVERIFY2(b->isVisible() && b->isEnabled(), qPrintable(what + " is not pressable"));
        clicks_ << what;
        QTest::mouseClick(b, Qt::LeftButton);
    }
    void writeReport() {
        const QString path = qEnvironmentVariable("PS2R_COLD_REPORT");
        if (path.isEmpty()) return;
        report_["wall_seconds"] = double(wall_.elapsed()) / 1000.0;
        report_["clicks"] = QJsonArray::fromStringList(clicks_);
        report_["commands_typed"] = 0;
        QFile f(path);
        if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(report_).toJson(QJsonDocument::Indented));
    }
    SetupSheet* sheet() const { return window_->setupSheet(); }

private slots:
    void initTestCase() {
        wall_.start();
        shots_ = qEnvironmentVariable("PS2R_COLD_SCREENSHOTS");
        report_["flow"] = "prebuilt-install";
        LauncherStyle::apply();
        app_ = new LauncherApp(this);
        QString err;
        QVERIFY2(app_->loadManifest(qEnvironmentVariable("PS2R_COLD_MANIFEST"), &err), qPrintable(err));
        report_["home"] = AppData::homePath();
        report_["app_data"] = app_->appData()->root();
        report_["releases_url"] = app_->installer()->releasesUrl();
        window_ = new MainWindow(app_);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_));
    }
    void cleanupTestCase() {
        writeReport();
        if (app_->game()->running()) app_->game()->stop();
        delete window_;
    }

    void whereIsYourGame() {
        QElapsedTimer t;
        t.start();
        QVERIFY2(window_->startSetupIfNeeded(), "the setup did not open: something is installed already (HOME is not cold)");
        QVERIFY(sheet()->isOpen());
        QCOMPARE(sheet()->step(), 0);
        QVERIFY2(QTest::qWaitFor([&] { return !sheet()->discStep()->looking() && !sheet()->discStep()->cards().isEmpty(); }, 180000),
                 "no disc image was found and identified in the usual folders");
        const DiscFound d = sheet()->discStep()->cards().first()->disc();
        report_["disc"] = d.path;
        report_["disc_region"] = d.region;
        report_["disc_serial"] = d.serial;
        // the identity badge after the whole-image check ("PAL · modified disc" for the development image: no known retail dump
        // matches it; "PAL · original disc" for a retail one) -- the fixture says which this disc must show
        report_["disc_state"] = sheet()->discStep()->cards().first()->identityBadgeText();
        report_["disc_verdict"] = d.verdict;
        report_["disc_image_sha1"] = d.imageSha1;
        report_["identify_seconds"] = double(t.elapsed()) / 1000.0;
        QCOMPARE(d.region, QStringLiteral("PAL"));
        QVERIFY2(!d.checking && d.checked, "the disc's whole-image check did not finish");
        if (!sheet()->discStep()->hasChoice()) QTest::mouseClick(sheet()->discStep()->cards().first(), Qt::LeftButton);
        QVERIFY(sheet()->discStep()->hasChoice());
        shot("01-where-is-your-game");
        press(sheet()->primaryButton(), "Next (Where is your game?)");
        QCOMPARE(sheet()->step(), 1);
        writeReport();
    }
    void whereShouldTheGameLive() {
        const QString folder = sheet()->folderStep()->folder();
        report_["game_folder"] = folder;
        QCOMPARE(folder, QDir::cleanPath(AppData::homePath() + "/Games/Crash Twinsanity"));
        QVERIFY2(sheet()->folderStep()->valid(), qPrintable(sheet()->folderStep()->problem()));
        shot("02-where-should-the-game-live");
        press(sheet()->primaryButton(), "Next (Where should the game live?)");
        QCOMPARE(sheet()->step(), 2);
    }
    void whereWillYouPlay() {
        QVERIFY(sheet()->targetStep()->thisComputer()->chosen());
        QCOMPARE(sheet()->targetStep()->target(), app_->hostTarget());
        report_["target"] = app_->hostTarget();
        shot("03-where-will-you-play");
        press(sheet()->primaryButton(), "Install (Where will you play?)");
        QCOMPARE(sheet()->step(), 3);
    }
    void install() {
        QElapsedTimer t;
        t.start();
        bool shotMid = false;
        QVERIFY2(QTest::qWaitFor([&] {
            if (!shotMid && app_->installer()->phase() == ReleaseInstaller::Downloading && app_->installer()->received() > 0) { shot("04-installing"); shotMid = true; }
            return sheet()->installStep()->done() || sheet()->installStep()->failed();
        }, envInt("PS2R_COLD_INSTALL_TIMEOUT_S", 600) * 1000), "the install did not finish in time");
        report_["install_seconds"] = double(t.elapsed()) / 1000.0;
        report_["installed_tag"] = app_->installer()->tag();
        report_["install_error"] = sheet()->installStep()->errorText();
        report_["download_bytes"] = double(app_->installer()->downloadedBytes());
        report_["asset_bytes"] = double(app_->installer()->total());
        QJsonArray listing;
        const QString folder = sheet()->installStep()->folder();
        QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext()) listing << QDir(folder).relativeFilePath(it.next());
        report_["game_folder_listing"] = listing;
        report_["bundled_mods"] = QJsonArray::fromStringList(app_->installer()->bundledMods());
        QFile ini(folder + "/ps2r.ini");
        if (ini.open(QIODevice::ReadOnly)) report_["ps2r_ini"] = QString::fromUtf8(ini.readAll());
        QFile disc(folder + "/disc-image.txt");
        if (disc.open(QIODevice::ReadOnly)) report_["disc_image_txt"] = QString::fromUtf8(disc.readAll()).trimmed();
        writeReport();
        QVERIFY2(sheet()->installStep()->done(), qPrintable(sheet()->installStep()->errorText()));
        QCOMPARE(sheet()->primaryButton()->text(), QStringLiteral("Play"));
        shot("05-ready");
    }
    void play() {
        QElapsedTimer t;
        t.start();
        press(sheet()->primaryButton(), "Play");
        QVERIFY(QTest::qWaitFor([&] { return app_->game()->running(); }, 15000));
        report_["play_log"] = app_->game()->logPath();
        report_["play_arguments"] = QJsonArray::fromStringList(app_->game()->lastArguments());
        const bool reached = QTest::qWaitFor([&] { return app_->game()->reachedTitle() || !app_->game()->running(); },
                                             envInt("PS2R_COLD_TITLE_TIMEOUT_S", 300) * 1000);
        report_["title_reached"] = app_->game()->reachedTitle();
        report_["title_seconds"] = double(t.elapsed()) / 1000.0;
        shot("06-playing");
        writeReport();
        QVERIFY2(reached && app_->game()->reachedTitle(), "the game did not reach its title state");
        app_->game()->stop();
        QVERIFY(QTest::qWaitFor([&] { return !app_->game()->running(); }, 20000));
        QVERIFY(!app_->library()->games().isEmpty() && app_->library()->games().first().lastPlayed.isValid());
        writeReport();
    }
    // the launcher's own update: the check made when the window opened (startSetupIfNeeded) found the newer launcher
    void launcherUpdate() {
        SelfUpdate* su = app_->selfUpdate();
        QVERIFY2(QTest::qWaitFor([&] { return su->state() != SelfUpdate::Idle && su->state() != SelfUpdate::Checking; }, 30000),
                 "the launcher's own check did not finish");
        QJsonObject u;
        u["state"] = int(su->state());
        u["available"] = su->state() == SelfUpdate::Available;
        u["tag"] = su->release().tag;
        u["asset"] = su->release().asset.name;
        u["running_version"] = su->running().version;
        u["running_kind"] = su->running().kind;
        u["download_page"] = su->downloadPage().toString();
        QString why;
        u["cannot_replace"] = int(su->cannotReplace(&why));
        u["cannot_replace_text"] = why;
        window_->showPage(MainWindow::GamesIndex);
        QTest::qWait(200);
        LauncherUpdateBox* games = window_->gamesPage()->launcherUpdateBox();
        u["games_visible"] = games->isVisible();
        u["games_line"] = games->text();
        u["games_update_button"] = games->updateButton()->isVisible();
        u["games_download_button"] = games->downloadButton()->isVisible();
        shot("07-launcher-update-games");
        window_->showPage(MainWindow::AboutIndex);
        QTest::qWait(200);
        LauncherUpdateBox* about = window_->aboutPage()->launcherUpdateBox();
        u["about_line"] = about->text();
        u["about_download_button"] = about->downloadButton()->isVisible();
        shot("08-launcher-update-about");
        // the game's own Update is the game's: the launcher's release does not offer it
        u["game_update_offered"] = app_->updateFor(app_->hostTarget());
        u["log"] = su->logPath();
        report_["launcher_update"] = u;
        writeReport();
        QVERIFY2(su->state() == SelfUpdate::Available, qPrintable(QStringLiteral("no newer launcher was offered (state %1)").arg(int(su->state()))));
        QVERIFY(u["games_visible"].toBool());            // seen while the Games page was shown
        QVERIFY(u["games_download_button"].toBool());
        window_->showPage(MainWindow::GamesIndex);
    }
};

QTEST_MAIN(TestLauncherCold)
#include "test_launcher_cold.moc"
