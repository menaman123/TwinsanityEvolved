// app/launcher/game_card.cpp -- the game card, painted.
#include "game_card.h"
#include <QContextMenuEvent>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QVBoxLayout>
#include "badge.h"
#include "disc_text.h"
#include "flat_button.h"
#include "folder_row.h"
#include "game_library.h"
#include "game_process.h"
#include "generated_artwork.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
constexpr int kCardHeight = 172;
constexpr int kArt = 140;

QString minutesText(qint64 seconds) {
    if (seconds < 60) return QStringLiteral("%1 s").arg(seconds);
    return QStringLiteral("%1 min").arg(seconds / 60);
}
}  // namespace

GameCard::GameCard(LauncherApp* app, const QString& isoPath, QWidget* parent) : QWidget(parent), app_(app), iso_(isoPath) {
    const LauncherTheme& t = LauncherTheme::instance();
    setMinimumHeight(kCardHeight);
    setAccessibleName(QStringLiteral("%1 card").arg(app->manifest()->name()));
    const GameEntry* g = app->library()->game(isoPath);
    auto* art = new GeneratedArtwork(app->manifest()->name(), app->manifest()->id() + isoPath, kArt, this);
    title_ = new QLabel(app->manifest()->name(), this);
    title_->setFont(t.font(LauncherTheme::Title));
    region_ = new Badge(g ? DiscText::identityBadge(g->disc) : QString(), Badge::Neutral, this);
    state_ = new Badge(QString(), Badge::Neutral, this);
    details_ = new QLabel(this);
    details_->setFont(t.font(LauncherTheme::Body));
    status_ = new QLabel(this);
    status_->setFont(t.font(LauncherTheme::Body));
    status_->setWordWrap(true);
    facts_ = new QLabel(this);
    facts_->setFont(t.font(LauncherTheme::Caption));
    facts_->setWordWrap(true);
    action_ = new FlatButton(QStringLiteral("Play"), FlatButton::Primary, this);
    action_->setLarge(true);
    update_ = new FlatButton(QStringLiteral("Update"), FlatButton::Secondary, this);
    update_->hide();
    remove_ = new FlatButton(QStringLiteral("Remove"), FlatButton::Quiet, this);
    remove_->hide();
    auto* head = new QHBoxLayout();
    head->setSpacing(LauncherTheme::S2);
    head->addWidget(title_);
    head->addWidget(region_);
    head->addWidget(state_);
    head->addStretch(1);
    auto* actions = new QHBoxLayout();
    actions->setSpacing(LauncherTheme::S2);
    actions->addStretch(1);
    actions->addWidget(remove_);
    actions->addWidget(update_);
    actions->addWidget(action_);
    auto* text = new QVBoxLayout();
    text->setSpacing(LauncherTheme::S1);
    text->addLayout(head);
    text->addWidget(details_);
    text->addWidget(status_);
    text->addWidget(facts_);
    text->addStretch(1);
    text->addLayout(actions);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(LauncherTheme::S3, LauncherTheme::S3, LauncherTheme::S3, LauncherTheme::S3);
    row->setSpacing(LauncherTheme::S3);
    row->addWidget(art, 0, Qt::AlignTop);
    row->addLayout(text, 1);
    connect(action_, &QAbstractButton::clicked, this, [this] {
        switch (app_->nextAction(iso_)) {
        case LauncherApp::Missing: emit locateRequested(); break;
        case LauncherApp::NotInstalled: emit installRequested(); break;
        default: emit playRequested(); break;
        }
    });
    connect(update_, &QAbstractButton::clicked, this, &GameCard::updateRequested);
    connect(remove_, &QAbstractButton::clicked, this, &GameCard::removeRequested);
    connect(&t, &LauncherTheme::changed, this, [this] { restyle(); update(); });
    refresh();
}

QString GameCard::statusText() const { return status_->text(); }
QString GameCard::factsText() const { return facts_->text(); }
QString GameCard::stateBadgeText() const { return state_->text(); }
QString GameCard::identityBadgeText() const { return region_->text(); }

void GameCard::refresh() {
    const LauncherTheme& t = LauncherTheme::instance();
    const GameEntry* g = app_->library()->game(iso_);
    if (!g) return;
    const LauncherApp::Action next = app_->nextAction(iso_);
    const bool playable = next != LauncherApp::NotPlayable;
    region_->setText(DiscText::identityBadge(g->disc));
    region_->setTone(DiscText::identityTone(g->disc));
    state_->setText(DiscText::stateBadge(g->disc, playable));
    state_->setTone(DiscText::stateTone(g->disc, playable));
    state_->setVisible(!state_->text().isEmpty());
    details_->setText(QStringLiteral("%1 · %2").arg(DiscText::detailsLine(g->disc), DiscText::fileName(g->disc)));
    const InstallRecord here = app_->playableInstall();
    const QString newer = here.target.isEmpty() ? QString() : app_->updateFor(here.target);
    QString status, color = t.css(LauncherTheme::TextMuted);
    update_->setVisible(false);
    remove_->setVisible(false);
    action_->setVisible(true);
    switch (next) {
    case LauncherApp::Missing:
        status = QStringLiteral("The disc image is no longer at %1.").arg(g->disc.path);
        color = t.css(LauncherTheme::Error);
        action_->setText(QStringLiteral("Locate…"));
        action_->setKind(FlatButton::Secondary);
        remove_->setVisible(true);
        break;
    case LauncherApp::NotPlayable:
        status = app_->notPlayableReason(iso_);
        color = t.css(LauncherTheme::Warn);
        action_->setVisible(false);
        break;
    case LauncherApp::NotInstalled:
        status = QStringLiteral("The game is not installed on this computer yet.");
        action_->setText(QStringLiteral("Install…"));
        action_->setKind(FlatButton::Primary);
        break;
    case LauncherApp::Playing:
        status = QStringLiteral("Playing… %1").arg(minutesText(app_->game()->elapsedSeconds()));
        action_->setText(QStringLiteral("Playing…"));
        action_->setKind(FlatButton::Primary);
        break;
    case LauncherApp::Play:
        if (!newer.isEmpty()) status = QStringLiteral("A new version is ready: %1.").arg(newer);
        else if (!g->disc.warning.isEmpty()) { status = g->disc.warning; color = t.css(LauncherTheme::Warn); }
        else {
            // what the image is, in the words of the badge (a modified copy, the check still running); nothing for an original
            const QString verdict = DiscText::verdictLine(g->disc);
            status = verdict.isEmpty() ? QStringLiteral("Ready to play.") : QStringLiteral("Ready to play. %1").arg(verdict);
            color = t.css(LauncherTheme::Ok);
        }
        action_->setText(QStringLiteral("Play"));
        action_->setKind(FlatButton::Primary);
        if (!newer.isEmpty()) {
            update_->setText(QStringLiteral("Update to %1").arg(newer));
            update_->setAccessibleName(update_->text());
            update_->setVisible(true);
        }
        break;
    }
    action_->setEnabled(next != LauncherApp::Playing);
    action_->setAccessibleName(action_->text());
    status_->setText(status);
    status_->setStyleSheet(QStringLiteral("color: %1;").arg(color));
    QStringList facts;
    facts << GameLibrary::relativeTime(g->lastPlayed);
    if (g->playtimeSeconds > 0) facts << GameLibrary::playtimeText(g->playtimeSeconds);
    if (!here.tag.isEmpty()) facts << here.tag;
    facts_->setText(facts.join(QStringLiteral(" · ")));
    restyle();
}

void GameCard::restyle() {
    const LauncherTheme& t = LauncherTheme::instance();
    title_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    details_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    facts_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
}

void GameCard::paintEvent(QPaintEvent*) {
    const LauncherTheme& t = LauncherTheme::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(t.color(LauncherTheme::Line), 1));
    p.setBrush(t.color(LauncherTheme::Surface));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), LauncherTheme::radiusCard(), LauncherTheme::radiusCard());
}

void GameCard::contextMenuEvent(QContextMenuEvent* event) {
    QMenu menu(this);
    QAction* show = menu.addAction(QStringLiteral("Show the disc image"));
    QAction* remove = menu.addAction(QStringLiteral("Remove from the list"));
    QAction* chosen = menu.exec(event->globalPos());
    if (chosen == remove) emit removeRequested();
    else if (chosen == show) FolderRow::revealInFileManager(QFileInfo(iso_).absolutePath());
}

}  // namespace ps2r::launcher
