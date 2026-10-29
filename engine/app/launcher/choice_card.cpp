// app/launcher/choice_card.cpp -- the choice card, painted.
#include "choice_card.h"
#include <QKeyEvent>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>
#include "launcher_theme.h"
namespace ps2r::launcher {

ChoiceCard::ChoiceCard(const QString& title, const QString& line, QWidget* parent) : QWidget(parent) {
    const LauncherTheme& t = LauncherTheme::instance();
    setFocusPolicy(Qt::StrongFocus);
    setCursor(Qt::PointingHandCursor);
    setAccessibleName(title);
    title_ = new QLabel(title, this);
    title_->setFont(t.font(LauncherTheme::Label));
    line_ = new QLabel(line, this);
    line_->setFont(t.font(LauncherTheme::Body));
    line_->setWordWrap(true);
    detail_ = new QLabel(this);
    detail_->setFont(t.font(LauncherTheme::Caption));
    detail_->hide();
    col_ = new QVBoxLayout(this);
    col_->setContentsMargins(LauncherTheme::S3, LauncherTheme::S3, LauncherTheme::S3, LauncherTheme::S3);
    col_->setSpacing(LauncherTheme::S1);
    col_->addWidget(title_);
    col_->addWidget(line_);
    col_->addWidget(detail_);
    connect(&t, &LauncherTheme::changed, this, &ChoiceCard::restyle);
    restyle();
}

QString ChoiceCard::title() const { return title_->text(); }

void ChoiceCard::setDetail(const QString& detail) {
    detail_->setText(detail);
    detail_->setVisible(!detail.isEmpty());
}

void ChoiceCard::setExtra(QWidget* extra) {
    extra_ = extra;
    extra_->setParent(this);
    col_->addSpacing(LauncherTheme::S1);
    col_->addWidget(extra_, 0, Qt::AlignLeft);
    extra_->setVisible(chosen_);
}

void ChoiceCard::setChosen(bool chosen) {
    chosen_ = chosen;
    if (extra_) extra_->setVisible(chosen);
    setAccessibleDescription(chosen ? QStringLiteral("chosen") : QString());
    update();
}

void ChoiceCard::restyle() {
    const LauncherTheme& t = LauncherTheme::instance();
    title_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    line_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    detail_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    update();
}

void ChoiceCard::paintEvent(QPaintEvent*) {
    const LauncherTheme& t = LauncherTheme::instance();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(QPen(chosen_ ? t.color(LauncherTheme::Accent) : t.color(LauncherTheme::Line), chosen_ ? 2 : 1));
    p.setBrush(chosen_ ? t.color(LauncherTheme::AccentSoft) : (hover_ ? t.color(LauncherTheme::SurfaceRaised) : t.color(LauncherTheme::Surface)));
    p.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), LauncherTheme::radiusCard(), LauncherTheme::radiusCard());
    if (hasFocus()) {
        p.setPen(QPen(t.color(LauncherTheme::Accent), 1, Qt::DotLine));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(4, 4, -4, -4), LauncherTheme::radiusCard() - 2, LauncherTheme::radiusCard() - 2);
    }
}

void ChoiceCard::mousePressEvent(QMouseEvent* event) {
    if (event->button() != Qt::LeftButton) return;
    if (!chosen_) { setChosen(true); emit chosenByUser(); }
}

void ChoiceCard::keyPressEvent(QKeyEvent* event) {
    if (event->key() == Qt::Key_Space || event->key() == Qt::Key_Return || event->key() == Qt::Key_Enter) {
        if (!chosen_) { setChosen(true); emit chosenByUser(); }
        return;
    }
    QWidget::keyPressEvent(event);
}

void ChoiceCard::enterEvent(QEnterEvent*) { hover_ = true; update(); }
void ChoiceCard::leaveEvent(QEvent*) { hover_ = false; update(); }

}  // namespace ps2r::launcher
