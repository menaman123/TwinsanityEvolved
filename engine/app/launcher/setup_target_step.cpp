// app/launcher/setup_target_step.cpp -- step 3 of the setup.
#include "setup_target_step.h"
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include "choice_card.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "segmented_control.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

SetupTargetStep::SetupTargetStep(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    const TitleManifest* m = app->manifest();
    auto* title = new QLabel(QStringLiteral("Where will you play?"), this);
    title->setFont(t.font(LauncherTheme::Title));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    here_ = new ChoiceCard(QStringLiteral("This computer"), QStringLiteral("Install the game here and play it from the launcher."), this);
    here_->setObjectName(QStringLiteral("target_here"));
    here_->setDetail(m->targetLabel(app->hostTarget()));
    vita_ = new ChoiceCard(m->vitaLabel(), QStringLiteral("Get the PS Vita version to copy to your console."), this);
    vita_->setObjectName(QStringLiteral("target_vita"));
    other_ = new ChoiceCard(QStringLiteral("Another computer"), QStringLiteral("Get the game for a different computer and carry the folder over."), this);
    other_->setObjectName(QStringLiteral("target_other"));
    platforms_ = new SegmentedControl();
    platforms_->setAccessibleName(QStringLiteral("Which computer"));
    QStringList labels, ids;
    for (const ComputerTarget& c : m->computers()) {
        if (c.id == app->hostTarget()) continue;
        labels << c.label;
        ids << c.id;
    }
    platforms_->setItems(labels, ids);
    if (!ids.isEmpty()) platforms_->setCurrentIndex(0);
    other_->setExtra(platforms_);
    note_ = new QLabel(this);
    note_->setObjectName(QStringLiteral("target_vita_note"));
    note_->setFont(t.font(LauncherTheme::Body));
    note_->setWordWrap(true);
    note_->setTextFormat(Qt::RichText);
    note_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    note_->hide();
    // the cards and the PS Vita note scroll under the title: with PS Vita chosen the note (its steps, which disc the
    // console starts, the two discs side by side) is taller than the sheet leaves, and a squeezed label is drawn under
    // the next card
    auto* host = new QWidget();
    auto* cards = new QVBoxLayout(host);
    cards->setContentsMargins(0, 0, 0, 0);
    cards->setSpacing(LauncherTheme::S2);
    cards->addWidget(here_);
    cards->addWidget(vita_);
    cards->addWidget(note_);
    cards->addWidget(other_);
    cards->addStretch(1);
    auto* scroll = new QScrollArea(this);
    scroll->setObjectName(QStringLiteral("target_scroll"));
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    scroll->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    scroll->setWidget(host);
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(title);
    col->addSpacing(LauncherTheme::S1);
    col->addWidget(scroll, 1);
    for (ChoiceCard* c : {here_, vita_, other_}) connect(c, &ChoiceCard::chosenByUser, this, [this, c] { pickCard(c); });
    connect(platforms_, &SegmentedControl::valueChanged, this, [this](const QString&) { emit targetChanged(target()); });
    pickCard(here_);
}

void SetupTargetStep::pickCard(ChoiceCard* card) {
    for (ChoiceCard* c : {here_, vita_, other_}) c->setChosen(c == card);
    if (card == vita_) {   // the steps with the disc's name the PS Vita folder will give it
        QString html = QStringLiteral("<ol style=\"margin-left: -24px;\">");
        for (const QString& line : VitaFolder::steps(app_->manifest(), app_->vitaConsoleDisc()))
            html += QStringLiteral("<li>%1</li>").arg(line.toHtmlEscaped());
        html += QStringLiteral("</ol>");
        // which disc the console starts, and the North American disc beside the European one
        for (const QString& line : VitaFolder::discsNote(app_->manifest(), app_->vitaConsoleDisc()))
            html += QStringLiteral("<p>%1</p>").arg(line.toHtmlEscaped());
        note_->setText(html);
    }
    note_->setVisible(card == vita_);
    emit targetChanged(target());
}

QString SetupTargetStep::target() const {
    if (vita_->chosen()) return QStringLiteral("vita");
    if (other_->chosen() && !platforms_->currentValue().isEmpty()) return platforms_->currentValue();
    return app_->hostTarget();
}

void SetupTargetStep::setTarget(const QString& target) {
    if (target == QLatin1String("vita")) { pickCard(vita_); return; }
    if (target == app_->hostTarget()) { pickCard(here_); return; }
    platforms_->setCurrentValue(target);
    pickCard(other_);
}

QString SetupTargetStep::vitaNote() const { return note_->text(); }

}  // namespace ps2r::launcher
