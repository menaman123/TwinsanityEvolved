// app/launcher/games_page.cpp -- the Games page.
#include "games_page.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMimeData>
#include <QPainter>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include "app_data.h"
#include "disc_scanner.h"
#include "empty_state.h"
#include "flat_button.h"
#include "folder_row.h"
#include "game_card.h"
#include "game_library.h"
#include "game_process.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "launcher_update_box.h"
#include "page_header.h"
#include "progress_panel.h"
#include "release_catalogue.h"
#include "release_installer.h"
#include "title_manifest.h"
namespace ps2r::launcher {

GamesPage::GamesPage(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    setAcceptDrops(true);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    scroll_->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(LauncherTheme::S5, 0, LauncherTheme::S5, LauncherTheme::S5);
    col->setSpacing(LauncherTheme::S3);
    col->addWidget(new PageHeader(QStringLiteral("Games"), QStringLiteral("Your disc images, recognised by what is on them."), inner));
    // a newer launcher: offered here, where the launcher opens; hidden while there is nothing to say
    launcherUpdate_ = new LauncherUpdateBox(app, LauncherUpdateBox::OnGames, inner);
    col->addWidget(launcherUpdate_);
    banner_ = new QLabel(inner);
    banner_->setObjectName(QStringLiteral("games_banner"));
    banner_->setFont(t.font(LauncherTheme::Body));
    banner_->setWordWrap(true);
    QColor tint = t.color(LauncherTheme::Error);
    tint.setAlphaF(0.10f);
    banner_->setStyleSheet(QStringLiteral("color: %1; background: rgba(%2, %3, %4, %5); border-radius: 6px; padding: 8px 10px;")
                               .arg(t.css(LauncherTheme::Error)).arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alphaF()));
    banner_->hide();
    col->addWidget(banner_);
    cardsHost_ = new QWidget(inner);
    cardsLayout_ = new QVBoxLayout(cardsHost_);
    cardsLayout_->setContentsMargins(0, 0, 0, 0);
    cardsLayout_->setSpacing(LauncherTheme::S3);
    col->addWidget(cardsHost_);
    empty_ = new EmptyState(QStringLiteral("No disc image yet"),
                            QStringLiteral("Drop your disc image here, or choose it. The launcher recognises it by what is on it."),
                            QStringLiteral("Choose a disc image…"), QStringLiteral("Search again"), inner);
    empty_->setObjectName(QStringLiteral("empty_state"));
    col->addWidget(empty_);
    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(LauncherTheme::S2);
    searchNote_ = new QLabel(inner);
    searchNote_->setFont(t.font(LauncherTheme::Caption));
    searchNote_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    bottom->addWidget(searchNote_, 1);
    add_ = new FlatButton(QStringLiteral("Add a disc image…"), FlatButton::Secondary, inner);
    search_ = new FlatButton(QStringLiteral("Search again"), FlatButton::Quiet, inner);
    bottom->addWidget(add_);
    bottom->addWidget(search_);
    col->addLayout(bottom);
    // the installs and an update in progress
    auto* installedTitle = new QLabel(QStringLiteral("Installed"), inner);
    installedTitle->setFont(t.font(LauncherTheme::Title));
    installedTitle->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addSpacing(LauncherTheme::S2);
    col->addWidget(installedTitle);
    installsHost_ = new QWidget(inner);
    installsLayout_ = new QVBoxLayout(installsHost_);
    installsLayout_->setContentsMargins(0, 0, 0, 0);
    installsLayout_->setSpacing(LauncherTheme::S1);
    col->addWidget(installsHost_);
    updateProgress_ = new ProgressPanel(inner);
    updateProgress_->hide();
    col->addWidget(updateProgress_);
    updateLine_ = new QLabel(inner);
    updateLine_->setObjectName(QStringLiteral("games_update_line"));
    updateLine_->setFont(t.font(LauncherTheme::Body));
    updateLine_->setWordWrap(true);
    updateLine_->hide();
    col->addWidget(updateLine_);
    another_ = new FlatButton(QStringLiteral("Get the game for PS Vita or another computer…"), FlatButton::Secondary, inner);
    col->addWidget(another_, 0, Qt::AlignLeft);
    col->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll_);

    connect(empty_, &EmptyState::primaryClicked, this, &GamesPage::chooseDisc);
    connect(empty_, &EmptyState::quietClicked, this, [this] { app_->scan(); });
    connect(add_, &QAbstractButton::clicked, this, &GamesPage::chooseDisc);
    connect(search_, &QAbstractButton::clicked, this, [this] {
        searchNote_->setText(QStringLiteral("Looking for disc images…"));
        app_->scan();
    });
    connect(another_, &QAbstractButton::clicked, this, &GamesPage::anotherPlaceRequested);
    connect(app_->library(), &GameLibrary::changed, this, &GamesPage::rebuildCards);
    connect(app_, &LauncherApp::stateChanged, this, &GamesPage::refreshCards);
    connect(app_, &LauncherApp::installsChanged, this, &GamesPage::rebuildInstalls);
    connect(app_, &LauncherApp::updateAvailable, this, [this](const QString&, const QString&) { rebuildInstalls(); refreshCards(); });
    connect(app_->scanner(), &DiscScanner::found, this, [this](const DiscFound& d) {
        if (!app_->setupDone()) return;   // the setup's step 1 chooses; afterwards a found disc is a card
        if (!locating_.isEmpty()) {   // Locate…: the card follows the disc to where it is now
            app_->library()->relocate(locating_, d);
            locating_.clear();
            return;
        }
        app_->addDisc(d);
        banner_->hide();
    });
    connect(app_->scanner(), &DiscScanner::finished, this, [this](int n, int) {
        if (!app_->setupDone()) return;
        searchNote_->setText(n == 0 ? QStringLiteral("No disc image of %1 was found in the usual places.").arg(app_->manifest()->name())
                                    : QStringLiteral("%1 disc image%2 found.").arg(n).arg(n == 1 ? "" : "s"));
    });
    connect(app_, &LauncherApp::discRefused, this, [this](const QString& path, const QString& reason) {
        if (!app_->setupDone()) return;
        locating_.clear();
        showRefusal(path, reason);
    });
    ReleaseInstaller* inst = app_->installer();
    connect(inst, &ReleaseInstaller::progress, this, [this](qint64 received, qint64 total, double rate) {
        if (updating_.isEmpty()) return;
        updateProgress_->setProgress(int(received >> 10), int(qMax<qint64>(1, total) >> 10));
        updateProgress_->setCaption(QStringLiteral("%1 of %2 · %3").arg(ReleaseCatalogue::bytesText(received), ReleaseCatalogue::bytesText(total),
                                                                       ReleaseCatalogue::rateText(rate)));
    });
    connect(inst, &ReleaseInstaller::finished, this, [this](bool ok) {
        if (updating_.isEmpty()) return;
        const LauncherTheme& th = LauncherTheme::instance();
        updateProgress_->clear();
        updateProgress_->hide();
        updateLine_->setStyleSheet(QStringLiteral("color: %1;").arg(th.css(ok ? LauncherTheme::Ok : LauncherTheme::Error)));
        updateLine_->setText(ok ? QStringLiteral("Updated to %1. Your settings, saves and mods were kept.").arg(app_->installer()->tag())
                                : QStringLiteral("The update did not finish. %1").arg(app_->installer()->failureText()));
        updateLine_->show();
        updating_.clear();
        rebuildInstalls();
        refreshCards();
    });
    rebuildCards();
    rebuildInstalls();
}

GameCard* GamesPage::card(const QString& isoPath) const {
    for (GameCard* c : cards_) if (c->isoPath() == isoPath) return c;
    return nullptr;
}

QString GamesPage::bannerText() const { return banner_->isVisible() ? banner_->text() : QString(); }
QString GamesPage::updateText() const { return updateLine_->isVisible() ? updateLine_->text() : QString(); }

void GamesPage::rebuildCards() {
    for (GameCard* c : cards_) { cardsLayout_->removeWidget(c); c->deleteLater(); }
    cards_.clear();
    for (const GameEntry& g : app_->library()->games()) {
        auto* c = new GameCard(app_, g.disc.path, cardsHost_);
        c->setObjectName(QStringLiteral("card_%1").arg(cards_.size()));
        connect(c, &GameCard::playRequested, this, [this, path = g.disc.path] { play(path); });
        connect(c, &GameCard::installRequested, this, &GamesPage::installHereRequested);
        connect(c, &GameCard::updateRequested, this, [this] { startUpdate(app_->hostTarget()); });
        connect(c, &GameCard::locateRequested, this, [this, path = g.disc.path] { locate(path); });
        connect(c, &GameCard::removeRequested, this, [this, path = g.disc.path] { app_->library()->remove(path); });
        cards_ << c;
        cardsLayout_->addWidget(c);
    }
    empty_->setVisible(cards_.isEmpty());
    if (cards_.isEmpty()) {
        empty_->setTone(EmptyState::Neutral);
        empty_->setTexts(QStringLiteral("No disc image yet"),
                         QStringLiteral("Drop your disc image here, or choose it. The launcher recognises it by what is on it."));
    }
    cardsHost_->setVisible(!cards_.isEmpty());
    add_->setVisible(!cards_.isEmpty());
    search_->setVisible(!cards_.isEmpty());
}

void GamesPage::rebuildInstalls() {
    const LauncherTheme& t = LauncherTheme::instance();
    while (QLayoutItem* item = installsLayout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    const QVector<InstallRecord> all = app_->installs();
    if (all.isEmpty()) {
        auto* none = new QLabel(QStringLiteral("Nothing is installed yet."), installsHost_);
        none->setFont(t.font(LauncherTheme::Body));
        none->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
        installsLayout_->addWidget(none);
        return;
    }
    for (const InstallRecord& r : all) {
        auto* w = new QWidget(installsHost_);
        w->setObjectName(QStringLiteral("install_%1").arg(r.target));
        auto* row = new QHBoxLayout(w);
        row->setContentsMargins(0, LauncherTheme::S1, 0, LauncherTheme::S1);
        row->setSpacing(LauncherTheme::S2);
        const QString where = r.target == app_->hostTarget() ? QStringLiteral("This computer") : app_->manifest()->targetLabel(r.target);
        auto* label = new QLabel(QStringLiteral("<b>%1</b> · %2 · %3").arg(where.toHtmlEscaped(), r.tag.toHtmlEscaped(), AppData::shortPath(r.folder).toHtmlEscaped()), w);
        label->setFont(t.font(LauncherTheme::Body));
        label->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
        row->addWidget(label, 1);
        const QString newer = app_->updateFor(r.target);
        if (!newer.isEmpty() && r.target != app_->hostTarget()) {
            auto* up = new FlatButton(QStringLiteral("Update to %1").arg(newer), FlatButton::Secondary, w);
            connect(up, &QAbstractButton::clicked, this, [this, target = r.target] { startUpdate(target); });
            row->addWidget(up);
        }
        auto* open = new FlatButton(FolderRow::openInFileManagerLabel(), FlatButton::Quiet, w);
        open->setAccessibleName(QStringLiteral("Open %1's folder").arg(where));
        connect(open, &QAbstractButton::clicked, this, [folder = r.folder] { FolderRow::revealInFileManager(folder); });
        row->addWidget(open);
        installsLayout_->addWidget(w);
    }
}

void GamesPage::refreshCards() {
    for (GameCard* c : cards_) c->refresh();
}

void GamesPage::startUpdate(const QString& target) {
    const InstallRecord r = app_->installFor(target);
    if (r.target.isEmpty() || app_->installer()->running()) return;
    updating_ = target;
    updateLine_->hide();
    updateProgress_->show();
    updateProgress_->setText(QStringLiteral("Updating %1 to %2…").arg(app_->manifest()->targetLabel(target), app_->updateFor(target)));
    updateProgress_->setIndeterminate(true);
    app_->startInstall(target, r.folder);
}

void GamesPage::play(const QString& isoPath) {
    QString err;
    if (!app_->play(isoPath, &err)) showRefusal(isoPath, err);
}

void GamesPage::chooseDisc() {
    QStringList filters;
    for (const QString& e : app_->manifest()->discExtensions()) filters << "*" + e;
    const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose your %1 disc image").arg(app_->manifest()->name()), start,
                                                      QStringLiteral("Disc images (%1)").arg(filters.join(' ')));
    if (!path.isEmpty()) addDiscFromPath(path);
}

void GamesPage::locate(const QString& isoPath) {
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Where is the disc image now?"), QFileInfo(isoPath).absolutePath());
    if (path.isEmpty()) return;
    locating_ = isoPath;
    app_->identify(path);
}

void GamesPage::addDiscFromPath(const QString& path) {
    banner_->hide();
    app_->identify(path);
}

void GamesPage::showRefusal(const QString& path, const QString& reason) {
    const QString title = QStringLiteral("%1 cannot be used.").arg(QFileInfo(path).fileName());
    if (cards_.isEmpty()) {
        empty_->setTone(EmptyState::Error);
        empty_->setTexts(title, reason);
        empty_->setVisible(true);
        return;
    }
    banner_->setText(QStringLiteral("%1 %2").arg(title, reason));
    banner_->show();
}

void GamesPage::showEvent(QShowEvent* event) {
    app_->library()->refreshMissing();
    refreshCards();
    QWidget::showEvent(event);
}

void GamesPage::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasUrls()) { event->acceptProposedAction(); dragOver_ = true; update(); }
}

void GamesPage::dragLeaveEvent(QDragLeaveEvent*) { dragOver_ = false; update(); }

void GamesPage::dropEvent(QDropEvent* event) {
    dragOver_ = false;
    update();
    for (const QUrl& u : event->mimeData()->urls()) {
        if (u.isLocalFile()) { addDiscFromPath(u.toLocalFile()); break; }
    }
    event->acceptProposedAction();
}

void GamesPage::paintEvent(QPaintEvent*) {
    if (!dragOver_) return;
    const LauncherTheme& t = LauncherTheme::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(t.color(LauncherTheme::Accent), 2, Qt::DashLine));
    p.setBrush(Qt::NoBrush);
    p.drawRoundedRect(QRectF(rect()).adjusted(LauncherTheme::S3, LauncherTheme::S3, -LauncherTheme::S3, -LauncherTheme::S3), LauncherTheme::radiusCard(), LauncherTheme::radiusCard());
    p.setFont(t.font(LauncherTheme::Title));
    p.setPen(t.color(LauncherTheme::Accent));
    p.drawText(rect().adjusted(0, 0, 0, -LauncherTheme::S5), Qt::AlignHCenter | Qt::AlignBottom, QStringLiteral("Drop the disc image to add it"));
}

}  // namespace ps2r::launcher
