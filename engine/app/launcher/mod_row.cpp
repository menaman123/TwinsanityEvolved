// app/launcher/mod_row.cpp -- the mod row.
#include "mod_row.h"
#include <QApplication>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include "badge.h"
#include "flat_button.h"
#include "launcher_theme.h"
#include "switch_control.h"
namespace ps2r::launcher {

namespace {
constexpr int kHandleWidth = 16;

QLabel* lineLabel(QWidget* parent, LauncherTheme::TextStyle style) {
    auto* l = new QLabel(parent);
    l->setFont(LauncherTheme::instance().font(style));
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    l->hide();
    return l;
}
}  // namespace

ModRow::ModRow(const QString& id, const QString& name, QWidget* parent) : QWidget(parent), id_(id) {
    const LauncherTheme& t = LauncherTheme::instance();
    setObjectName(QStringLiteral("mod_%1").arg(id));
    setMouseTracking(true);   // the handle's hand cursor
    name_ = new QLabel(name, this);
    name_->setFont(t.font(LauncherTheme::Label));
    region_ = new Badge(QString(), Badge::Neutral, this);
    region_->hide();
    byline_ = lineLabel(this, LauncherTheme::Caption);
    summary_ = lineLabel(this, LauncherTheme::Caption);
    warning_ = lineLabel(this, LauncherTheme::Caption);
    warning_->setObjectName(QStringLiteral("mod_warning"));
    notes_ = lineLabel(this, LauncherTheme::Caption);
    notes_->setObjectName(QStringLiteral("mod_notes"));
    actions_ = new QWidget(this);
    auto* actions = new QHBoxLayout(actions_);
    actions->setContentsMargins(0, 0, 0, 0);
    actions->setSpacing(LauncherTheme::S1);
    up_ = new FlatButton(QStringLiteral("Move up"), FlatButton::Quiet, actions_);
    down_ = new FlatButton(QStringLiteral("Move down"), FlatButton::Quiet, actions_);
    open_ = new FlatButton(QStringLiteral("Open folder"), FlatButton::Quiet, actions_);
    remove_ = new FlatButton(QStringLiteral("Remove…"), FlatButton::Quiet, actions_);
    for (FlatButton* b : {up_, down_, open_, remove_}) {
        b->setAccessibleName(QStringLiteral("%1: %2").arg(b->text(), name));
        actions->addWidget(b);
    }
    actions->addStretch(1);
    actions_->hide();
    switch_ = new SwitchControl(this);
    switch_->setAccessibleName(name);
    auto* head = new QHBoxLayout();
    head->setSpacing(LauncherTheme::S2);
    head->addWidget(name_);
    head->addWidget(region_);
    head->addStretch(1);
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, LauncherTheme::S2, 0, LauncherTheme::S2);
    grid->setHorizontalSpacing(LauncherTheme::S3);
    grid->setVerticalSpacing(2);
    grid->addLayout(head, 0, 0);
    grid->addWidget(byline_, 1, 0);
    grid->addWidget(summary_, 2, 0);
    grid->addWidget(warning_, 3, 0);
    grid->addWidget(notes_, 4, 0);
    grid->addWidget(actions_, 5, 0);
    grid->addWidget(switch_, 0, 1, 6, 1, Qt::AlignRight | Qt::AlignVCenter);
    grid->setColumnStretch(0, 1);
    connect(switch_, &QAbstractButton::toggled, this, [this](bool on) { emit switched(id_, on); });
    connect(up_, &QAbstractButton::clicked, this, [this] { emit moveRequested(id_, -1); });
    connect(down_, &QAbstractButton::clicked, this, [this] { emit moveRequested(id_, +1); });
    connect(open_, &QAbstractButton::clicked, this, [this] { emit openRequested(id_); });
    connect(remove_, &QAbstractButton::clicked, this, [this] { emit removeRequested(id_); });
    connect(&t, &LauncherTheme::changed, this, &ModRow::restyle);
    restyle();
}

void ModRow::restyle() {
    const LauncherTheme& t = LauncherTheme::instance();
    name_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    byline_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    summary_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(broken_ ? LauncherTheme::Error : LauncherTheme::TextMuted)));
    warning_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Warn)));
    notes_->setStyleSheet(QStringLiteral("color: %1; font-style: italic;").arg(t.css(LauncherTheme::TextMuted)));
    update();
}

void ModRow::setByline(const QString& text) { byline_->setText(text); byline_->setVisible(!text.isEmpty()); }
void ModRow::setSummary(const QString& text) { summary_->setText(text); summary_->setVisible(!text.isEmpty()); }
void ModRow::setWarning(const QString& text) { warning_->setText(text); warning_->setVisible(!text.isEmpty()); }
void ModRow::setNotes(const QString& text) { notes_->setText(text); notes_->setVisible(!text.isEmpty()); }

void ModRow::setRegion(const QString& label) {
    region_->setText(label);
    region_->setVisible(!label.isEmpty());
}

void ModRow::setSwitchable(bool switchable) {
    switchable_ = switchable;
    switch_->setVisible(switchable && !broken_);
}

void ModRow::setFolderMod(bool folderMod) {
    folderMod_ = folderMod;
    actions_->setVisible(folderMod);
    const QMargins m = layout()->contentsMargins();
    layout()->setContentsMargins(folderMod ? kHandleWidth + LauncherTheme::S2 : 0, m.top(), m.right(), m.bottom());
    up_->setVisible(folderMod && !broken_);
    down_->setVisible(folderMod && !broken_);
    update();
}

void ModRow::setMovable(bool up, bool down) {
    up_->setEnabled(up);
    down_->setEnabled(down);
}

void ModRow::setOn(bool on) {
    switch_->blockSignals(true);
    switch_->setChecked(on);
    switch_->blockSignals(false);
}

bool ModRow::on() const { return switch_->isChecked(); }

void ModRow::setBroken(const QString& why) {
    broken_ = true;
    setSummary(why);
    setWarning(QString());
    setRegion(QString());
    switch_->setEnabled(false);
    switch_->hide();
    up_->hide();
    down_->hide();
    restyle();
}

QString ModRow::nameText() const { return name_->text(); }
QString ModRow::bylineText() const { return byline_->isHidden() ? QString() : byline_->text(); }
QString ModRow::summaryText() const { return summary_->isHidden() ? QString() : summary_->text(); }
QString ModRow::warningText() const { return warning_->isHidden() ? QString() : warning_->text(); }
QString ModRow::notesText() const { return notes_->isHidden() ? QString() : notes_->text(); }
QString ModRow::regionText() const { return region_->isHidden() ? QString() : region_->text(); }

QRect ModRow::handleRect() const { return folderMod_ && !broken_ ? QRect(0, 0, kHandleWidth, height()) : QRect(); }

void ModRow::paintEvent(QPaintEvent*) {
    const LauncherTheme& t = LauncherTheme::instance();
    QPainter p(this);
    p.setPen(QPen(t.color(LauncherTheme::Line), 1));
    p.drawLine(0, height() - 1, width(), height() - 1);
    const QRect h = handleRect();
    if (h.isEmpty()) return;
    // the handle: two columns of three dots beside the name
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(t.color(LauncherTheme::TextFaint));
    const int top = name_->geometry().center().y() - 6;
    for (int r = 0; r < 3; ++r)
        for (int c = 0; c < 2; ++c) p.drawEllipse(QPointF(h.left() + 5 + c * 6, top + r * 6), 1.6, 1.6);
}

void ModRow::mousePressEvent(QMouseEvent* event) {
    pressed_ = event->button() == Qt::LeftButton && handleRect().contains(event->position().toPoint());
    pressAt_ = event->position().toPoint();
    if (pressed_) setCursor(Qt::ClosedHandCursor);
    QWidget::mousePressEvent(event);
}

void ModRow::mouseMoveEvent(QMouseEvent* event) {
    if (!pressed_) {
        setCursor(handleRect().contains(event->position().toPoint()) ? Qt::OpenHandCursor : Qt::ArrowCursor);
    } else if ((event->position().toPoint() - pressAt_).manhattanLength() >= QApplication::startDragDistance()) {
        pressed_ = false;
        unsetCursor();
        emit dragRequested(id_);
        return;
    }
    QWidget::mouseMoveEvent(event);
}

void ModRow::mouseReleaseEvent(QMouseEvent* event) {
    pressed_ = false;
    unsetCursor();
    QWidget::mouseReleaseEvent(event);
}

}  // namespace ps2r::launcher
