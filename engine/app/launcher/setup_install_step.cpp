// app/launcher/setup_install_step.cpp -- step 4 of the setup.
#include "setup_install_step.h"
#include <QLabel>
#include <QVBoxLayout>
#include "app_data.h"
#include "flat_button.h"
#include "folder_row.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "progress_panel.h"
#include "release_catalogue.h"
#include "release_installer.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

namespace {
QString timeLeft(qint64 left, double rate) {
    if (rate <= 0 || left <= 0) return QString();
    const qint64 s = qint64(double(left) / rate + 0.5);
    if (s < 60) return QStringLiteral(" · about %1 s left").arg(qMax<qint64>(1, s));
    return QStringLiteral(" · about %1 min left").arg((s + 30) / 60);
}
}  // namespace

SetupInstallStep::SetupInstallStep(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    title_ = new QLabel(QStringLiteral("Installing"), this);
    title_->setFont(t.font(LauncherTheme::Title));
    title_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    line_ = new QLabel(this);
    line_->setFont(t.font(LauncherTheme::Body));
    line_->setWordWrap(true);
    line_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    progress_ = new ProgressPanel(this);
    error_ = new QLabel(this);
    error_->setObjectName(QStringLiteral("setup_install_error"));
    error_->setFont(t.font(LauncherTheme::Body));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Error)));
    error_->hide();
    note_ = new QLabel(this);
    note_->setFont(t.font(LauncherTheme::Body));
    note_->setWordWrap(true);
    note_->setTextFormat(Qt::RichText);
    note_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    note_->hide();
    show_ = new FlatButton(QStringLiteral("Show the folder"), FlatButton::Quiet, this);
    show_->hide();
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(title_);
    col->addWidget(line_);
    col->addSpacing(LauncherTheme::S2);
    col->addWidget(progress_);
    col->addWidget(error_);
    col->addWidget(note_);
    col->addWidget(show_, 0, Qt::AlignLeft);
    col->addStretch(1);
    connect(show_, &QAbstractButton::clicked, this, [this] { FolderRow::revealInFileManager(folder_); });
    ReleaseInstaller* inst = app_->installer();
    connect(inst, &ReleaseInstaller::phaseChanged, this, &SetupInstallStep::onPhase);
    connect(inst, &ReleaseInstaller::finished, this, &SetupInstallStep::onFinished);
    connect(inst, &ReleaseInstaller::progress, this, [this](qint64 received, qint64 total, double rate) {
        if (!isVisible() && target_.isEmpty()) return;
        progress_->setProgress(int(received >> 10), int(qMax<qint64>(total, 1) >> 10));
        progress_->setCaption(QStringLiteral("%1 of %2 · %3%4").arg(ReleaseCatalogue::bytesText(received), ReleaseCatalogue::bytesText(total),
                                                                  ReleaseCatalogue::rateText(rate), timeLeft(total - received, rate)));
    });
}

bool SetupInstallStep::running() const { return app_->installer()->running(); }

void SetupInstallStep::start(const QString& target, const QString& folder) {
    target_ = target;
    folder_ = folder;
    done_ = failed_ = false;
    title_->setText(QStringLiteral("Installing"));
    line_->setText(QStringLiteral("%1 for %2, into %3.").arg(app_->manifest()->name(), app_->manifest()->targetLabel(target), AppData::shortPath(folder)));
    error_->hide();
    note_->hide();
    show_->hide();
    progress_->show();
    progress_->setText(QStringLiteral("Looking for the latest version…"));
    progress_->setCaption(QString());
    progress_->setIndeterminate(true);
    emit stateChanged();
    app_->startInstall(target, folder);
}

void SetupInstallStep::retry() { start(target_, folder_); }

void SetupInstallStep::onPhase(int phase) {
    const ReleaseInstaller* inst = app_->installer();
    const QString what = QStringLiteral("%1 %2 for %3").arg(app_->manifest()->name(), inst->tag(), app_->manifest()->targetLabel(target_));
    switch (phase) {
    case ReleaseInstaller::Looking: progress_->setText(QStringLiteral("Looking for the latest version…")); progress_->setIndeterminate(true); break;
    case ReleaseInstaller::Downloading: progress_->setText(QStringLiteral("Downloading %1").arg(what)); break;
    case ReleaseInstaller::Checking: progress_->setText(QStringLiteral("Checking the download…")); progress_->setIndeterminate(true); break;
    case ReleaseInstaller::Placing:
        progress_->setText(QStringLiteral("Putting the game in %1…").arg(AppData::shortPath(folder_)));
        progress_->setIndeterminate(true);
        break;
    default: break;
    }
    emit stateChanged();
}

void SetupInstallStep::onFinished(bool ok) {
    if (target_.isEmpty()) return;
    const TitleManifest* m = app_->manifest();
    progress_->clear();
    progress_->hide();
    if (!ok) {
        failed_ = true;
        title_->setText(QStringLiteral("The game was not installed"));
        error_->setText(app_->installer()->failureText());
        error_->show();
        emit stateChanged();
        return;
    }
    done_ = true;
    const QString tag = app_->installer()->tag();
    show_->show();
    if (target_ == QLatin1String("vita")) {
        // the PS Vita folder: the package, the README and the console's data folder, ready to copy
        const VitaFolderReport v = app_->lastVitaFolder();
        title_->setText(QStringLiteral("The %1 folder is ready.").arg(m->vitaLabel()));
        line_->setText(QStringLiteral("%1 %2, its data folder and the steps are in %3.").arg(m->vitaFile(), tag, AppData::shortPath(folder_)));
        QString html = QStringLiteral("<ol style=\"margin-left: -24px;\">");
        const QString disc = v.disc.isEmpty() ? app_->vitaConsoleDisc() : v.disc;
        for (const QString& l : VitaFolder::steps(m, disc)) html += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
        html += QStringLiteral("</ol>");
        for (const QString& l : VitaFolder::discsNote(m, disc)) html += QStringLiteral("<p>%1</p>").arg(l.toHtmlEscaped());
        if (!v.dataDir.isEmpty()) html += QStringLiteral("<p>%1</p>").arg(VitaFolder::summary(v).toHtmlEscaped());
        note_->setText(html);
        note_->show();
        if (!v.error.isEmpty()) {
            error_->setText(QStringLiteral("The data folder is not complete: %1").arg(v.error));
            error_->show();
        }
    } else if (target_ != app_->hostTarget()) {
        const ComputerTarget* c = m->computer(target_);
        title_->setText(QStringLiteral("The game for %1 is ready.").arg(c ? c->label : target_));
        line_->setText(QStringLiteral("%1 is in %2.").arg(tag, AppData::shortPath(folder_)));
        note_->setText(QStringLiteral("Copy %1 to the other computer and open %2 there. The first time, it asks for your disc image.")
                           .arg(AppData::shortPath(folder_).toHtmlEscaped(), c ? c->exe : m->binaryName()));
        note_->show();
    } else {
        title_->setText(QStringLiteral("%1 is ready.").arg(m->name()));
        line_->setText(QStringLiteral("%1 is in %2.").arg(tag, AppData::shortPath(folder_)));
    }
    emit stateChanged();
}

void SetupInstallStep::showProgressForScreenshot(qint64 received, qint64 total, double rate) {
    progress_->show();
    progress_->setText(QStringLiteral("Downloading %1 v0.3.0 for %2").arg(app_->manifest()->name(), app_->manifest()->targetLabel(target_)));
    progress_->setProgress(int(received >> 10), int(total >> 10));
    progress_->setCaption(QStringLiteral("%1 of %2 · %3%4").arg(ReleaseCatalogue::bytesText(received), ReleaseCatalogue::bytesText(total),
                                                              ReleaseCatalogue::rateText(rate), timeLeft(total - received, rate)));
}

QString SetupInstallStep::statusText() const { return title_->text(); }
QString SetupInstallStep::errorText() const { return error_->isVisible() ? error_->text() : QString(); }

}  // namespace ps2r::launcher
