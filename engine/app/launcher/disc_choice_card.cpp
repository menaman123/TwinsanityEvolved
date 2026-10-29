// app/launcher/disc_choice_card.cpp -- the disc card of the setup, painted.
#include "disc_choice_card.h"
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QVBoxLayout>
#include "badge.h"
#include "disc_text.h"
#include "generated_artwork.h"
#include "launcher_theme.h"
namespace ps2r::launcher {

namespace {
constexpr int kArt = 64;
}

DiscChoiceCard::DiscChoiceCard(const QString& titleName, const DiscFound& disc, bool playable, const QString& notPlayable, QWidget* parent)
    : QWidget(parent), disc_(disc), playable_(playable), notPlayable_(notPlayable) {
    const LauncherTheme& t = LauncherTheme::instance();
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(QStringLiteral("%1, %2").arg(titleName, DiscText::fileName(disc)));
    auto* art = new GeneratedArtwork(titleName, disc.serial + disc.path, kArt, this);
    title_ = new QLabel(titleName, this);
    title_->setFont(t.font(LauncherTheme::Label));
    identity_ = new Badge(QString(), Badge::Neutral, this);
    stateBadge_ = new Badge(QString(), Badge::Warn, this);
    details_ = new QLabel(DiscText::detailsLine(disc), this);
    details_->setFont(t.font(LauncherTheme::Caption));
    state_ = new QLabel(this);
    state_->setFont(t.font(LauncherTheme::Caption));
    state_->setWordWrap(true);
    auto* head = new QHBoxLayout();
    head->setSpacing(LauncherTheme::S2);
    head->addWidget(title_);
    head->addWidget(identity_);
    head->addWidget(stateBadge_);
    head->addStretch(1);
    auto* text = new QVBoxLayout();
    text->setSpacing(LauncherTheme::S1);
    text->addLayout(head);
    text->addWidget(details_);
    text->addWidget(state_);
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(LauncherTheme::S3, LauncherTheme::S2, LauncherTheme::S5, LauncherTheme::S2);
    row->setSpacing(LauncherTheme::S3);
    row->addWidget(art, 0, Qt::AlignVCenter);
    row->addLayout(text, 1);
    connect(&t, &LauncherTheme::changed, this, &DiscChoiceCard::restyle);
    showDisc();
    restyle();
}

void DiscChoiceCard::setDisc(const DiscFound& disc) {
    disc_ = disc;
    showDisc();
}

void DiscChoiceCard::showDisc() {
    identity_->setText(DiscText::identityBadge(disc_));
    identity_->setTone(DiscText::identityTone(disc_));
    stateBadge_->setText(DiscText::stateBadge(disc_, playable_));
    stateBadge_->setTone(DiscText::stateTone(disc_, playable_));
    stateBadge_->setVisible(!stateBadge_->text().isEmpty());
    details_->setText(DiscText::detailsLine(disc_));
    state_->setText(DiscText::stateLine(disc_, playable_, notPlayable_));
    state_->setVisible(!state_->text().isEmpty());
}

QString DiscChoiceCard::identityBadgeText() const { return identity_->text(); }
QString DiscChoiceCard::stateBadgeText() const { return stateBadge_->text(); }
QString DiscChoiceCard::stateLineText() const { return state_->text(); }

void DiscChoiceCard::setChosen(bool chosen) {
    chosen_ = chosen;
    setAccessibleDescription(chosen ? QStringLiteral("chosen") : QStringLiteral("not chosen"));
    update();
}

void DiscChoiceCard::restyle() {
    const LauncherTheme& t = LauncherTheme::instance();
    title_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    details_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    state_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    update();
}

void DiscChoiceCard::paintEvent(QPaintEvent*) {
    const LauncherTheme& t = LauncherTheme::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(chosen_ ? t.color(LauncherTheme::Accent) : t.color(LauncherTheme::Line), chosen_ ? 2 : 1));
    p.setBrush(chosen_ ? t.color(LauncherTheme::AccentSoft) : t.color(LauncherTheme::Surface));
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), LauncherTheme::radiusCard(), LauncherTheme::radiusCard());
    // the check: a ring, filled with a tick when chosen
    const QRectF ring(width() - LauncherTheme::S3 - 18, (height() - 18) / 2.0, 18, 18);
    p.setPen(QPen(chosen_ ? t.color(LauncherTheme::Accent) : t.color(LauncherTheme::TextFaint), 1.5));
    p.setBrush(chosen_ ? t.color(LauncherTheme::Accent) : Qt::transparent);
    p.drawEllipse(ring);
    if (chosen_) {
        QPainterPath tick;
        tick.moveTo(ring.left() + 4.5, ring.center().y());
        tick.lineTo(ring.left() + 7.8, ring.bottom() - 5);
        tick.lineTo(ring.right() - 4.2, ring.top() + 5.5);
        p.setPen(QPen(t.color(LauncherTheme::AccentText), 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
        p.setBrush(Qt::NoBrush);
        p.drawPath(tick);
    }
    if (hasFocus()) {
        p.setPen(QPen(t.color(LauncherTheme::Accent), 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(4, 4, -4, -4), LauncherTheme::radiusCard() - 2, LauncherTheme::radiusCard() - 2);
    }
}

void DiscChoiceCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    setChosen(!chosen_);
    emit toggled(chosen_);
}

void DiscChoiceCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space) {
        setChosen(!chosen_);
        emit toggled(chosen_);
        return;
    }
    QWidget::keyPressEvent(event);
}

}  // namespace ps2r::launcher
