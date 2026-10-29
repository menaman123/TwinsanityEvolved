// app/launcher/setup_disc_step.cpp -- step 1 of the setup.
#include "setup_disc_step.h"
#include "app/host/disc_regions.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QScrollArea>
#include <QStandardPaths>
#include <QVBoxLayout>
#include "app_data.h"
#include "disc_choice_card.h"
#include "empty_state.h"
#include "flat_button.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "progress_panel.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
QString placesText() { return QStringLiteral("Downloads, Desktop, Documents, the launcher's folder and removable drives"); }
}

SetupDiscStep::SetupDiscStep(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* title = new QLabel(QStringLiteral("Where is your game?"), this);
    title->setFont(t.font(LauncherTheme::Title));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    auto* intro = new QLabel(QStringLiteral("Choose your %1 disc image. The launcher looks in Downloads, Desktop, Documents, its own "
                                            "folder and any removable drive, and recognises the disc by what is on it.").arg(app->manifest()->name()), this);
    intro->setFont(t.font(LauncherTheme::Body));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    banner_ = new QLabel(this);
    banner_->setFont(t.font(LauncherTheme::Body));
    banner_->setWordWrap(true);
    banner_->setObjectName(QStringLiteral("setup_disc_banner"));
    QColor tint = t.color(LauncherTheme::Error);
    tint.setAlphaF(0.10f);
    banner_->setStyleSheet(QStringLiteral("color: %1; background: rgba(%2, %3, %4, %5); border-radius: 6px; padding: 8px 10px;")
                               .arg(t.css(LauncherTheme::Error)).arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alphaF()));
    banner_->hide();
    progress_ = new ProgressPanel(this);
    cardsHost_ = new QWidget();
    cardsLayout_ = new QVBoxLayout(cardsHost_);
    cardsLayout_->setContentsMargins(0, 0, 0, 0);
    cardsLayout_->setSpacing(LauncherTheme::S2);
    cardsLayout_->addStretch(1);
    scroll_ = new QScrollArea(this);
    scroll_->setWidgetResizable(true);
    scroll_->setFrameShape(QFrame::NoFrame);
    scroll_->setWidget(cardsHost_);
    scroll_->setMinimumHeight(196);
    empty_ = new EmptyState(QStringLiteral("No disc image found"),
                            QStringLiteral("Looked in %1. Drop your disc image on this window, or choose it.").arg(placesText()),
                            QStringLiteral("Choose a disc image…"), QStringLiteral("Search again"), this);
    empty_->setObjectName(QStringLiteral("setup_disc_empty"));
    empty_->setCompact(true);
    empty_->hide();
    choose_ = new FlatButton(QStringLiteral("Choose a disc image…"), FlatButton::Secondary, this);
    again_ = new FlatButton(QStringLiteral("Search again"), FlatButton::Quiet, this);
    auto* buttons = new QHBoxLayout();
    buttons->setSpacing(LauncherTheme::S2);
    buttons->addWidget(choose_);
    buttons->addWidget(again_);
    buttons->addStretch(1);
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(title);
    col->addWidget(intro);
    col->addSpacing(LauncherTheme::S1);
    col->addWidget(banner_);
    col->addWidget(progress_);
    col->addWidget(scroll_, 1);
    col->addWidget(empty_, 1);
    col->addLayout(buttons);
    connect(choose_, &QAbstractButton::clicked, this, &SetupDiscStep::choose);
    connect(again_, &QAbstractButton::clicked, this, &SetupDiscStep::search);
    connect(empty_, &EmptyState::primaryClicked, this, &SetupDiscStep::choose);
    connect(empty_, &EmptyState::quietClicked, this, &SetupDiscStep::search);
    DiscScanner* scanner = app_->scanner();
    connect(scanner, &DiscScanner::checking, this, [this](const QString& path, qint64 done, qint64 total) {
        if (!isVisible() && !searching_) return;
        checking_ = true;
        progress_->show();
        progress_->setText(QStringLiteral("Checking %1…").arg(QFileInfo(path).fileName()));
        progress_->setProgress(int(done >> 20), int(total >> 20));
        if (searching_) progress_->setCaption(QStringLiteral("Looking in %1.").arg(placesText()));
        else progress_->setCaption(QString());
    });
    connect(scanner, &DiscScanner::found, this, [this](const DiscFound& d) {
        if (!begun_) return;
        const bool wanted = d.path == wantChosen_;
        addCard(d, wanted);
        if (wanted) { wantChosen_.clear(); banner_->hide(); }
        if (!app_->scanner()->running()) checking_ = false;
        refresh();
    });
    // the whole-image check finished: the card takes the verdict; an original, playable disc is chosen when none is
    connect(scanner, &DiscScanner::checked, this, [this](const DiscFound& d) {
        if (!begun_) return;
        for (DiscChoiceCard* c : cards_) {
            if (c->disc().path != d.path) continue;
            DiscFound next = d;
            next.fromSearch = c->disc().fromSearch;
            c->setDisc(next);
            bool anyChosen = false;
            for (DiscChoiceCard* o : cards_) anyChosen |= o->chosen();
            if (!anyChosen && c->playable() && d.retail) { c->setChosen(true); emit choiceChanged(); }
        }
        if (!app_->scanner()->running()) checking_ = false;
        refresh();
    });
    connect(scanner, &DiscScanner::finished, this, [this](int, int) {
        searching_ = false;
        checking_ = app_->scanner()->running();
        refresh();
    });
    connect(app_, &LauncherApp::discRefused, this, [this](const QString& path, const QString& reason) {
        if (!begun_) return;
        checking_ = app_->scanner()->running();
        banner_->setText(QStringLiteral("%1 cannot be used. %2").arg(QFileInfo(path).fileName(), reason));
        banner_->show();
        wantChosen_.clear();
        refresh();
    });
    refresh();
}

void SetupDiscStep::begin() {
    if (begun_) return;
    begun_ = true;
    search();
}

void SetupDiscStep::search() {
    begun_ = true;
    searching_ = true;
    banner_->hide();
    progress_->show();
    progress_->setText(QStringLiteral("Looking for your disc image…"));
    progress_->setCaption(QStringLiteral("Looking in %1.").arg(placesText()));
    progress_->setIndeterminate(true);
    refresh();
    app_->scan();
}

void SetupDiscStep::showLookingForScreenshot() {
    searching_ = true;
    progress_->show();
    progress_->setText(QStringLiteral("Looking for your disc image…"));
    progress_->setCaption(QStringLiteral("Looking in %1.").arg(placesText()));
    progress_->setIndeterminate(true);
    refresh();
}

bool SetupDiscStep::looking() const { return searching_ || checking_; }
QString SetupDiscStep::bannerText() const { return banner_->isVisible() ? banner_->text() : QString(); }

void SetupDiscStep::choose() {
    QStringList filters;
    for (const QString& e : app_->manifest()->discExtensions()) filters << "*" + e;
    const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose your %1 disc image").arg(app_->manifest()->name()), start,
                                                      QStringLiteral("Disc images (%1)").arg(filters.join(' ')));
    if (!path.isEmpty()) addFromPath(path);
}

void SetupDiscStep::addFromPath(const QString& path) {
    begun_ = true;
    banner_->hide();
    const QString abs = QFileInfo(path).absoluteFilePath();
    for (DiscChoiceCard* c : cards_) {
        if (c->disc().path == abs) {
            c->setChosen(true);
            emit choiceChanged();
            return;
        }
    }
    wantChosen_ = abs;
    checking_ = true;
    progress_->show();
    progress_->setText(QStringLiteral("Checking %1…").arg(QFileInfo(abs).fileName()));
    progress_->setCaption(QString());
    progress_->setIndeterminate(true);
    refresh();
    app_->identify(abs);
}

void SetupDiscStep::addCard(const DiscFound& disc, bool choose) {
    for (DiscChoiceCard* c : cards_) {
        if (c->disc().path == disc.path) {
            if (!disc.checking || c->disc().checking) c->setDisc(disc);   // a finished verdict is not replaced by a quick one
            if (choose) { c->setChosen(true); emit choiceChanged(); }
            return;
        }
    }
    const bool playable = app_->manifest()->playableExecutables().isEmpty() ||
                          app_->manifest()->playableExecutables().contains(disc.executable, Qt::CaseInsensitive);
    // the reason another release does not play: the region table's sentence (the game's own refusal), else the manifest's note
    const std::string region = ps2r::app::RegionNotPlayable(disc.serial.toStdString());
    const QString notPlayable = region.empty() ? app_->manifest()->playsNote() : QString::fromStdString(region);
    auto* card = new DiscChoiceCard(app_->manifest()->name(), disc, playable, notPlayable, cardsHost_);
    card->setObjectName(QStringLiteral("setup_disc_%1").arg(cards_.size()));
    // the first original, playable disc is chosen for the user; a file they chose is always chosen
    bool anyChosen = false;
    for (DiscChoiceCard* c : cards_) anyChosen |= c->chosen();
    if (choose || (!anyChosen && playable && disc.retail)) card->setChosen(true);
    connect(card, &DiscChoiceCard::toggled, this, [this](bool) { emit choiceChanged(); });
    cards_ << card;
    cardsLayout_->insertWidget(cardsLayout_->count() - 1, card);
    emit choiceChanged();
}

void SetupDiscStep::refresh() {
    const bool busy = searching_ || checking_;
    if (!busy) { progress_->clear(); progress_->hide(); }
    const bool none = cards_.isEmpty();
    empty_->setVisible(none && !busy && begun_);
    scroll_->setVisible(!none || (busy && !empty_->isVisible()));
    choose_->setVisible(!empty_->isVisible());
    again_->setVisible(!empty_->isVisible());
    again_->setEnabled(!searching_);
}

QVector<DiscFound> SetupDiscStep::chosen() const {
    QVector<DiscFound> out;
    for (DiscChoiceCard* c : cards_) if (c->chosen()) out << c->disc();
    return out;
}

bool SetupDiscStep::hasChoice() const {
    for (DiscChoiceCard* c : cards_) if (c->chosen()) return true;
    return false;
}

}  // namespace ps2r::launcher
