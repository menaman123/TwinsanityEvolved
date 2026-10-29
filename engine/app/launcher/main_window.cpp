// app/launcher/main_window.cpp -- the window.
#include "main_window.h"
#include <QAction>
#include <QCoreApplication>
#include <QCloseEvent>
#include <QDateTime>
#include <QDir>
#include <QHBoxLayout>
#include <QJsonObject>
#include <QKeySequence>
#include <QStackedWidget>
#include "about_page.h"
#include "app_data.h"
#include "details_sheet.h"
#include "folder_row.h"
#include "game_process.h"
#include "games_page.h"
#include "launcher_app.h"
#include "launcher_style.h"
#include "launcher_theme.h"
#include "mods_page.h"
#include "self_update.h"
#include "settings_page.h"
#include "setup_sheet.h"
#include "sidebar.h"
#include "title_manifest.h"
namespace ps2r::launcher {

MainWindow::MainWindow(LauncherApp* app, QWidget* parent) : QMainWindow(parent), app_(app) {
    setWindowTitle(app->manifest()->product());
    setMinimumSize(1024, 640);
    auto* central = new QWidget(this);
    auto* row = new QHBoxLayout(central);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(0);
    sidebar_ = new Sidebar(app->manifest()->product(), QStringLiteral("Launcher %1").arg(app->version()),
                           {QStringLiteral("Games"), QStringLiteral("Mods"), QStringLiteral("Settings"), QStringLiteral("About")}, central);
    content_ = new QWidget(central);
    stack_ = new QStackedWidget(content_);
    auto* contentCol = new QHBoxLayout(content_);
    contentCol->setContentsMargins(0, 0, 0, 0);
    contentCol->addWidget(stack_);
    details_ = new DetailsSheet(content_);
    games_ = new GamesPage(app, stack_);
    mods_ = new ModsPage(app, stack_);
    settings_ = new SettingsPage(app, stack_);
    about_ = new AboutPage(app, details_, stack_);
    stack_->addWidget(games_);
    stack_->addWidget(mods_);
    stack_->addWidget(settings_);
    stack_->addWidget(about_);
    setup_ = new SetupSheet(app, content_);
    row->addWidget(sidebar_);
    row->addWidget(content_, 1);
    setCentralWidget(central);
    connect(sidebar_, &Sidebar::selected, this, &MainWindow::showPage);
    connect(sidebar_, &Sidebar::openRequested, this, [this] { FolderRow::revealInFileManager(app_->gameFolder()); });
    connect(games_, &GamesPage::anotherPlaceRequested, this, &MainWindow::openSetupForAnotherPlace);
    connect(games_, &GamesPage::installHereRequested, this, [this] { setup_->open(SetupSheet::AnotherPlace, app_->hostTarget()); });
    connect(mods_, &ModsPage::setupRequested, this, [this] { setup_->open(SetupSheet::FirstRun); });
    connect(mods_, &ModsPage::vitaSetupRequested, this, [this] { setup_->open(SetupSheet::AnotherPlace, QStringLiteral("vita")); });
    connect(settings_, &SettingsPage::setupRequested, this, [this] { setup_->open(SetupSheet::FirstRun); });
    connect(setup_, &SetupSheet::playRequested, this, [this](const QString& iso) {
        showPage(GamesIndex);
        games_->play(iso);
    });
    connect(setup_, &SetupSheet::closed, this, [this] { refreshFooter(); games_->rebuildCards(); });
    connect(app_, &LauncherApp::installsChanged, this, &MainWindow::refreshFooter);
    // the launcher's own update put the new copy in place: this window closes, the new launcher opens
    connect(app_->selfUpdate(), &SelfUpdate::quitRequested, this, [this] {
        close();                      // the window's state saved (closeEvent)
        QCoreApplication::quit();     // even with a dialog open
    }, Qt::QueuedConnection);
    for (int i = 0; i < 4; ++i) {
        auto* a = new QAction(this);
        a->setShortcut(QKeySequence(QStringLiteral("Ctrl+%1").arg(i + 1)));
        connect(a, &QAction::triggered, this, [this, i] { showPage(i); });
        addAction(a);
    }
    auto* shot = new QAction(QStringLiteral("Save a screenshot"), this);
    shot->setShortcut(QKeySequence(QStringLiteral("Ctrl+Shift+S")));
    connect(shot, &QAction::triggered, this, [this] { screenshot(); });
    addAction(shot);
    connect(&LauncherTheme::instance(), &LauncherTheme::changed, this, [] { LauncherStyle::apply(); });
    const QJsonObject win = app->appData()->value("window").toObject();
    const int w = qMax(1024, win.value("width").toInt(1120)), h = qMax(640, win.value("height").toInt(720));
    resize(w, h);
    showPage(qBound(0, win.value("page").toInt(0), 3));
    refreshFooter();
}

MainWindow::~MainWindow() = default;

void MainWindow::refreshFooter() { sidebar_->setFooter(QStringLiteral("game folder"), app_->gameFolder()); }

void MainWindow::showPage(int index) {
    stack_->setCurrentIndex(index);
    sidebar_->setCurrent(index);
}

int MainWindow::currentPage() const { return stack_->currentIndex(); }

bool MainWindow::startSetupIfNeeded() {
    app_->checkLauncherUpdate();   // a newer launcher is offered on the Games page, set up or not
    if (app_->setupDone()) {
        app_->checkForUpdates();
        return false;
    }
    showPage(GamesIndex);
    setup_->open(SetupSheet::FirstRun);
    return true;
}

void MainWindow::openSetupForAnotherPlace() { setup_->open(SetupSheet::AnotherPlace); }

QString MainWindow::screenshot(const QString& path) {
    QString out = path;
    if (out.isEmpty()) {
        QDir().mkpath(app_->appData()->logsDir());
        out = app_->appData()->logsDir() + "/screenshot-" + QDateTime::currentDateTime().toString("yyyyMMdd-HHmmss") + ".png";
    }
    grab().save(out, "PNG");
    return out;
}

void MainWindow::closeEvent(QCloseEvent* event) {
    QJsonObject win;
    win["width"] = width();
    win["height"] = height();
    win["page"] = stack_->currentIndex();
    app_->appData()->setValue("window", win);
    app_->appData()->save();
    if (app_->game()->running()) app_->game()->stop();
    QMainWindow::closeEvent(event);
}

}  // namespace ps2r::launcher
