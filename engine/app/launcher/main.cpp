// app/launcher/main.cpp -- the launcher's entry: the style, the manifest, the window; `--manifest PATH` names the
// title manifest (else titles/*.json beside the application), `--no-scan` opens without looking for disc images,
// `--home DIR` uses DIR as the home folder (the launcher's own data, the folders it searches, the game it starts: HOME is
// set for the process -- an argument, because macOS's `open` passes arguments to an app but not the environment),
// `--update-launcher` replaces this launcher with the newest published one as soon as it is found (the window's
// "Update the launcher" without the click), `--version` prints the version. `--after-update` is what an update starts the
// new launcher with (the clean-up runs at every start; the flag only says why it started), and on Windows / Linux
// `--wait-for <process>` (the old launcher's: nothing is read until it has exited). The update's relaunch keeps
// --home, --manifest and --no-scan.
#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>
#include "launcher_app.h"
#include "launcher_style.h"
#include "launcher_theme.h"
#include "launcher_version.h"
#include "main_window.h"
#include "self_update.h"
#include "title_manifest.h"

int main(int argc, char** argv) {
    QApplication::setApplicationName("Launcher");
    QApplication::setApplicationVersion(ps2r::launcher::launcherVersion());
    QApplication app(argc, argv);
    QCommandLineParser parser;
    parser.setApplicationDescription("The launcher: find your disc, install the game, press Play.");
    parser.addHelpOption();
    parser.addVersionOption();
    const QCommandLineOption manifestOpt("manifest", "The title manifest (title.json).", "path");
    const QCommandLineOption noScanOpt("no-scan", "Do not look for disc images at start.");
    const QCommandLineOption homeOpt("home", "Use this folder as the home folder: the launcher's own data and the places it looks in.", "folder");
    const QCommandLineOption updateOpt("update-launcher", "Replace this launcher with the newest published one, then open it again.");
    QCommandLineOption afterUpdateOpt("after-update", "Started by the launcher's own update.");   // lint: not-ui
    afterUpdateOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    QCommandLineOption waitOpt("wait-for", "Started by the launcher's own update: wait until that process has exited.", "process");   // lint: not-ui
    waitOpt.setFlags(QCommandLineOption::HiddenFromHelp);
    parser.addOption(manifestOpt);
    parser.addOption(noScanOpt);
    parser.addOption(homeOpt);
    parser.addOption(updateOpt);
    parser.addOption(afterUpdateOpt);
    parser.addOption(waitOpt);
    parser.process(app);
    // Windows / Linux: the launcher that updated itself started this one before it quit; nothing is read until it has
    // gone (its window's last save of launcher.json comes first), a minute at most (macOS: the shell that opens the new
    // bundle has waited already)
    if (parser.isSet(waitOpt)) ps2r::launcher::SelfUpdate::waitForExit(parser.value(waitOpt).toLongLong(), 60000);
    if (parser.isSet(homeOpt)) {
        // before anything reads it: the launcher's data folder, the search folders and the game's own lookups follow.
        // Its spelling on disk (a case-insensitive volume answers L2 for l2): the paths the launcher compares with it --
        // its own program's, the folders under it -- are spelled that way
        QString dir = QFileInfo(parser.value(homeOpt)).canonicalFilePath();
        if (dir.isEmpty()) dir = QDir(parser.value(homeOpt)).absolutePath();
        const QByteArray home = QFile::encodeName(dir);
        qputenv("HOME", home);   // lint: not-ui
#ifdef Q_OS_WIN
        qputenv("USERPROFILE", home);   // lint: not-ui
        qputenv("LOCALAPPDATA", home + "/AppData/Local");   // lint: not-ui
#endif
    }

    ps2r::launcher::LauncherTheme::instance();
    ps2r::launcher::LauncherStyle::apply();
    ps2r::launcher::LauncherApp launcher;
    QString error;
    if (!launcher.loadManifest(parser.value(manifestOpt), &error)) {
        // the one screen without a manifest: say what is missing
        QWidget w;
        w.setWindowTitle("Launcher");
        w.setMinimumSize(640, 240);
        auto* col = new QVBoxLayout(&w);
        col->setContentsMargins(32, 32, 32, 32);
        auto* title = new QLabel("This launcher has no game to launch", &w);
        title->setFont(ps2r::launcher::LauncherTheme::instance().font(ps2r::launcher::LauncherTheme::Title));
        auto* body = new QLabel(error + "\n\nThe launcher comes with its game's description beside it (titles/title.json). Download the launcher again.", &w);
        body->setWordWrap(true);
        col->addWidget(title);
        col->addWidget(body);
        col->addStretch(1);
        w.show();
        return app.exec();
    }
    QApplication::setApplicationName(launcher.manifest()->product());
    QApplication::setApplicationDisplayName(launcher.manifest()->product());
    if (parser.isSet(updateOpt)) launcher.selfUpdate()->setStartWhenFound(true);
    ps2r::launcher::MainWindow window(&launcher);
    window.show();
    if (!window.startSetupIfNeeded() && !parser.isSet(noScanOpt)) launcher.scan();
    return app.exec();
}
