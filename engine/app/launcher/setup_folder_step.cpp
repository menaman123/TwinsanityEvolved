// app/launcher/setup_folder_step.cpp -- step 2 of the setup.
#include "setup_folder_step.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QVBoxLayout>
#include "app_data.h"
#include "flat_button.h"
#include "folder_row.h"
#include "game_folder.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "release_catalogue.h"
#include "title_manifest.h"
namespace ps2r::launcher {

SetupFolderStep::SetupFolderStep(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    const TitleManifest* m = app->manifest();
    auto* title = new QLabel(QStringLiteral("Where should the game live?"), this);
    title->setFont(t.font(LauncherTheme::Title));
    title->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    auto* intro = new QLabel(QStringLiteral("Everything the game needs goes in one folder: the game, your settings, your memory cards and "
                                            "your mods. Move the folder and everything moves with it."), this);
    intro->setFont(t.font(LauncherTheme::Body));
    intro->setWordWrap(true);
    intro->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    row_ = new FolderRow(QStringLiteral("Game folder"), this);
    row_->setObjectName(QStringLiteral("setup_folder_row"));
    row_->changeButton()->setText(QStringLiteral("Choose another folder…"));
    row_->changeButton()->setAccessibleName(QStringLiteral("Choose another folder"));
    row_->openButton()->hide();
    contents_ = new QLabel(this);
    contents_->setFont(t.font(LauncherTheme::Body));
    contents_->setTextFormat(Qt::RichText);
    contents_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    const QString item = QStringLiteral("<tr><td style=\"padding-right:16px; color:%1;\"><b>%2</b></td><td>%3</td></tr>");
    contents_->setText(QStringLiteral("<table cellspacing=\"4\">") +
                       item.arg(t.css(LauncherTheme::Text), m->binaryName(), QStringLiteral("the game")) +
                       item.arg(t.css(LauncherTheme::Text), m->settingsFileName(), QStringLiteral("your settings")) +
                       item.arg(t.css(LauncherTheme::Text), m->memcardsName(), QStringLiteral("your saves")) +
                       item.arg(t.css(LauncherTheme::Text), m->modsName(), QStringLiteral("your mods")) + QStringLiteral("</table>"));
    space_ = new QLabel(this);
    space_->setFont(t.font(LauncherTheme::Caption));
    space_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    problem_ = new QLabel(this);
    problem_->setFont(t.font(LauncherTheme::Body));
    problem_->setWordWrap(true);
    problem_->setObjectName(QStringLiteral("setup_folder_problem"));
    problem_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Error)));
    problem_->hide();
    notice_ = new QLabel(this);
    notice_->setFont(t.font(LauncherTheme::Body));
    notice_->setWordWrap(true);
    notice_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Ok)));
    notice_->hide();
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(title);
    col->addWidget(intro);
    col->addSpacing(LauncherTheme::S2);
    col->addWidget(row_);
    col->addWidget(problem_);
    col->addWidget(notice_);
    col->addSpacing(LauncherTheme::S1);
    col->addWidget(contents_);
    col->addWidget(space_);
    col->addStretch(1);
    connect(row_, &FolderRow::changeRequested, this, &SetupFolderStep::pick);
    setFolder(GameFolder::suggested(m));
}

void SetupFolderStep::pick() {
    const QString start = QFileInfo(folder_).absolutePath();
    const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose where the game should live"), start);
    if (dir.isEmpty()) return;
    setFolder(GameFolder::resolveChoice(dir, app_->manifest()), true);
}

void SetupFolderStep::setFolder(const QString& folder, bool byUser) {
    folder_ = QDir::cleanPath(folder);
    byUser_ = byUser;
    row_->setPath(folder_);
    validate();
}

void SetupFolderStep::validate() {
    const qint64 need = app_->manifest()->installBytes();
    const qint64 free = GameFolder::freeBytes(folder_);
    QString problem;
    if (!GameFolder::writable(folder_)) problem = QStringLiteral("This folder cannot be written to. Choose another one.");
    else if (free >= 0 && free < need)
        problem = QStringLiteral("There is not enough free space here (the game needs about %1).").arg(ReleaseCatalogue::bytesText(need));
    problem_->setText(problem);
    problem_->setVisible(!problem.isEmpty());
    const bool already = problem.isEmpty() && GameFolder::holdsGame(folder_, app_->manifest());
    notice_->setText(already ? QStringLiteral("The game is already here: it will be updated, and your settings, saves and mods are kept.") : QString());
    notice_->setVisible(already);
    space_->setText(free >= 0 ? QStringLiteral("Needs about %1 · %2 free").arg(ReleaseCatalogue::bytesText(need), ReleaseCatalogue::bytesText(free))
                              : QStringLiteral("Needs about %1").arg(ReleaseCatalogue::bytesText(need)));
    const bool was = valid_;
    valid_ = problem.isEmpty();
    if (was != valid_) emit validityChanged(valid_);
}

QString SetupFolderStep::problem() const { return problem_->isVisible() ? problem_->text() : QString(); }
QString SetupFolderStep::notice() const { return notice_->isVisible() ? notice_->text() : QString(); }

}  // namespace ps2r::launcher
