// app/launcher/setup_sheet.cpp -- the setup sheet.
#include "setup_sheet.h"
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QLabel>
#include <QMimeData>
#include <QStackedWidget>
#include <QVBoxLayout>
#include "flat_button.h"
#include "game_folder.h"
#include "game_library.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "release_installer.h"
#include "setup_disc_step.h"
#include "setup_folder_step.h"
#include "setup_install_step.h"
#include "setup_target_step.h"
#include "sheet_overlay.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
const char* const kStepNames[] = {"Your game", "Game folder", "Where to play", "Install"};
}

SetupSheet::SetupSheet(LauncherApp* app, QWidget* host) : QWidget(host), app_(app) {
    hide();
    const LauncherTheme& t = LauncherTheme::instance();
    auto* content = new QWidget();
    content->setMinimumHeight(430);
    stepLine_ = new QLabel(content);
    stepLine_->setFont(t.font(LauncherTheme::Caption));
    stepLine_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Accent)));
    stack_ = new QStackedWidget(content);
    disc_ = new SetupDiscStep(app, stack_);
    folder_ = new SetupFolderStep(app, stack_);
    target_ = new SetupTargetStep(app, stack_);
    install_ = new SetupInstallStep(app, stack_);
    stack_->addWidget(disc_);
    stack_->addWidget(folder_);
    stack_->addWidget(target_);
    stack_->addWidget(install_);
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(stepLine_);
    col->addWidget(stack_, 1);
    overlay_ = new SheetOverlay(QStringLiteral("Set up %1").arg(app->manifest()->name()), content, host);
    overlay_->setPanelWidth(760);
    overlay_->setAcceptDrops(true);
    overlay_->installEventFilter(this);
    back_ = new FlatButton(QStringLiteral("Back"), FlatButton::Secondary);
    primary_ = new FlatButton(QStringLiteral("Next"), FlatButton::Primary);
    primary_->setLarge(true);
    overlay_->addButton(back_);
    overlay_->addButton(primary_);
    overlay_->setCancelButton(back_);
    connect(back_, &QAbstractButton::clicked, this, &SetupSheet::back);
    connect(primary_, &QAbstractButton::clicked, this, &SetupSheet::next);
    connect(disc_, &SetupDiscStep::choiceChanged, this, &SetupSheet::refreshButtons);
    connect(folder_, &SetupFolderStep::validityChanged, this, &SetupSheet::refreshButtons);
    connect(install_, &SetupInstallStep::stateChanged, this, &SetupSheet::refreshButtons);
    connect(target_, &SetupTargetStep::targetChanged, this, [this](const QString& t) {
        // a suggestion the user has not changed follows the target (another place's folder is named for it)
        if (mode_ == AnotherPlace && !folder_->chosenByUser()) folder_->setFolder(suggestedFolder(t));
    });
}

QString SetupSheet::suggestedFolder(const QString& target) const {
    const QString base = GameFolder::suggested(app_->manifest());
    if (mode_ == FirstRun || target == app_->hostTarget()) return base;
    // the PS Vita folder is made FROM the game folder, inside it: <game folder>/vita
    if (target == QLatin1String("vita")) return app_->vitaFolderFor(app_->gameFolder());
    const ComputerTarget* c = app_->manifest()->computer(target);
    return QStringLiteral("%1 (%2)").arg(base, c ? c->shortLabel : target);
}

void SetupSheet::open(Mode mode, const QString& target) {
    mode_ = mode;
    if (mode == FirstRun) {
        target_->setTarget(app_->hostTarget());
        const QString chosen = app_->gameFolder();
        if (!folder_->chosenByUser()) folder_->setFolder(chosen.isEmpty() ? GameFolder::suggested(app_->manifest()) : chosen, !chosen.isEmpty());
        go(0);
    } else {
        const QString t = target.isEmpty() ? QStringLiteral("vita") : target;
        target_->setTarget(t);
        folder_->setFolder(suggestedFolder(t));
        go(1);
    }
    overlay_->open();
}

void SetupSheet::close() {
    overlay_->close();
    emit closed();
}

bool SetupSheet::isOpen() const { return overlay_->isVisible(); }
int SetupSheet::step() const { return stack_->currentIndex(); }

QString SetupSheet::stepLine() const { return stepLine_->text(); }

void SetupSheet::go(int step) {
    stack_->setCurrentIndex(step);
    stepLine_->setText(QStringLiteral("Step %1 of 4 · %2").arg(step + 1).arg(QString::fromLatin1(kStepNames[step])));
    if (step == 0) disc_->begin();
    refreshButtons();
}

void SetupSheet::refreshButtons() {
    const int s = step();
    if (mode_ == AnotherPlace && s == 1) {   // opened at step 2: its Back leaves the sheet
        back_->setText(QStringLiteral("Cancel"));
        back_->setEnabled(true);
    } else {
        back_->setText(QStringLiteral("Back"));
        back_->setEnabled(s > 0);
    }
    switch (s) {
    case 0:
        primary_->setText(QStringLiteral("Next"));
        primary_->setEnabled(disc_->hasChoice());
        break;
    case 1:
        primary_->setText(QStringLiteral("Next"));
        primary_->setEnabled(folder_->valid());
        break;
    case 2:
        primary_->setText(QStringLiteral("Install"));
        primary_->setEnabled(true);
        break;
    default:
        if (install_->done()) primary_->setText(install_->target() == app_->hostTarget() ? QStringLiteral("Play") : QStringLiteral("Done"));
        else if (install_->failed()) primary_->setText(QStringLiteral("Try again"));
        else primary_->setText(QStringLiteral("Installing…"));
        primary_->setEnabled(install_->done() || install_->failed());
        break;
    }
    primary_->setAccessibleName(primary_->text());
    back_->setAccessibleName(back_->text());
}

void SetupSheet::next() {
    switch (step()) {
    case 0:
        if (!disc_->hasChoice()) return;
        app_->setDiscsChosen(disc_->chosen());
        go(1);
        break;
    case 1:
        if (!folder_->valid()) return;
        if (mode_ == FirstRun) app_->setChosenFolder(folder_->folder());
        go(2);
        break;
    case 2: {
        // the first run's folder is the game folder: a PS Vita chosen there goes into its vita folder
        const QString target = target_->target();
        const bool vitaHere = mode_ == FirstRun && target == QLatin1String("vita");
        go(3);
        install_->start(target, vitaHere ? app_->vitaFolderFor(folder_->folder()) : folder_->folder());
        break;
    }
    default:
        if (install_->failed()) { install_->retry(); return; }
        if (!install_->done()) return;
        if (install_->target() == app_->hostTarget()) {
            // Play: the first disc chosen in step 1 (or the first card)
            QString iso;
            const QVector<DiscFound> chosen = disc_->chosen();
            if (!chosen.isEmpty()) iso = chosen.first().path;
            else if (!app_->library()->games().isEmpty()) iso = app_->library()->games().first().disc.path;
            close();
            if (!iso.isEmpty()) emit playRequested(iso);
        } else {
            close();
        }
        break;
    }
}

void SetupSheet::back() {
    const int s = step();
    if (s == 3 && install_->running()) app_->installer()->cancel();
    if (mode_ == AnotherPlace && s <= 1) { close(); return; }
    if (s == 0) return;
    go(s - 1);
}

bool SetupSheet::eventFilter(QObject* watched, QEvent* event) {
    if (watched == overlay_) {
        if (event->type() == QEvent::DragEnter) {
            auto* e = static_cast<QDragEnterEvent*>(event);
            if (e->mimeData()->hasUrls()) e->acceptProposedAction();
            return true;
        }
        if (event->type() == QEvent::Drop) {
            auto* e = static_cast<QDropEvent*>(event);
            for (const QUrl& u : e->mimeData()->urls()) {
                if (!u.isLocalFile()) continue;
                if (!(step() == 3 && install_->running())) {
                    go(0);
                    disc_->addFromPath(u.toLocalFile());
                }
                break;
            }
            e->acceptProposedAction();
            return true;
        }
    }
    return QWidget::eventFilter(watched, event);
}

}  // namespace ps2r::launcher
