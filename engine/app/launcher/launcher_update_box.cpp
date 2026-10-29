// app/launcher/launcher_update_box.cpp -- the launcher's own update, as a box.
#include "launcher_update_box.h"
#include <QDesktopServices>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>
#include "flat_button.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "progress_panel.h"
#include "release_catalogue.h"
#include "self_update.h"
namespace ps2r::launcher {

LauncherUpdateBox::LauncherUpdateBox(LauncherApp* app, Place place, QWidget* parent) : QWidget(parent), app_(app), place_(place) {
    const LauncherTheme& t = LauncherTheme::instance();
    setObjectName(place == OnGames ? QStringLiteral("games_launcher_update") : QStringLiteral("about_launcher_update"));
    setAttribute(Qt::WA_StyledBackground, true);
    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(place == OnGames ? LauncherTheme::S3 : 0, place == OnGames ? LauncherTheme::S2 : 0,
                            place == OnGames ? LauncherTheme::S3 : 0, place == OnGames ? LauncherTheme::S2 : 0);
    col->setSpacing(LauncherTheme::S2);
    line_ = new QLabel(this);
    line_->setObjectName(objectName() + "_line");
    line_->setFont(t.font(LauncherTheme::Body));
    line_->setWordWrap(true);
    line_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(line_);
    progress_ = new ProgressPanel(this);
    progress_->hide();
    col->addWidget(progress_);
    auto* row = new QHBoxLayout();
    row->setSpacing(LauncherTheme::S2);
    update_ = new FlatButton(QStringLiteral("Update the launcher"), FlatButton::Primary, this);
    update_->setObjectName(objectName() + "_update");
    retry_ = new FlatButton(QStringLiteral("Try again"), FlatButton::Secondary, this);
    download_ = new FlatButton(QStringLiteral("Download it"), FlatButton::Secondary, this);
    download_->setObjectName(objectName() + "_download");
    check_ = new FlatButton(QStringLiteral("Check for a newer launcher"), FlatButton::Quiet, this);
    check_->setObjectName(objectName() + "_check");
    later_ = new FlatButton(QStringLiteral("Not now"), FlatButton::Quiet, this);
    for (FlatButton* b : {update_, retry_, download_, check_, later_}) row->addWidget(b);
    row->addStretch(1);
    col->addLayout(row);
    SelfUpdate* su = app_->selfUpdate();
    connect(update_, &QAbstractButton::clicked, su, &SelfUpdate::start);
    connect(retry_, &QAbstractButton::clicked, su, &SelfUpdate::start);
    connect(check_, &QAbstractButton::clicked, su, &SelfUpdate::check);
    connect(download_, &QAbstractButton::clicked, this, [su] { QDesktopServices::openUrl(su->downloadPage()); });
    connect(later_, &QAbstractButton::clicked, this, [this, su] {
        finishedSeen_ = true;   // the after-update line, if it was the one showing
        su->dismiss();
    });
    connect(su, &SelfUpdate::stateChanged, this, &LauncherUpdateBox::refresh);
    connect(su, &SelfUpdate::progress, this, [this](qint64 received, qint64 total, double rate) {
        progress_->setProgress(int(received >> 10), int(qMax<qint64>(1, total) >> 10));
        progress_->setCaption(QStringLiteral("%1 of %2 · %3").arg(ReleaseCatalogue::bytesText(received), ReleaseCatalogue::bytesText(total),
                                                                 ReleaseCatalogue::rateText(rate)));
    });
    // the game running or not changes whether the launcher may restart now
    connect(app_, &LauncherApp::stateChanged, this, &LauncherUpdateBox::refresh);
    connect(&t, &LauncherTheme::changed, this, &LauncherUpdateBox::restyle);
    restyle();
    refresh();
}

void LauncherUpdateBox::restyle() {
    const LauncherTheme& t = LauncherTheme::instance();
    if (place_ == OnGames) {
        QColor tint = t.color(LauncherTheme::Accent);
        tint.setAlphaF(0.10f);
        setStyleSheet(QStringLiteral("#%1 { background: rgba(%2, %3, %4, %5); border-radius: 6px; }")
                          .arg(objectName()).arg(tint.red()).arg(tint.green()).arg(tint.blue()).arg(tint.alphaF()));
    }
    refresh();
}

QString LauncherUpdateBox::text() const { return isVisible() || place_ == OnAbout ? line_->text() : QString(); }

void LauncherUpdateBox::refresh() {
    const LauncherTheme& t = LauncherTheme::instance();
    const SelfUpdate* su = app_->selfUpdate();
    const SelfUpdate::State s = su->state();
    QString why;
    const SelfUpdate::Problem blocked = su->cannotReplace(&why);
    const bool busy = s == SelfUpdate::Downloading || s == SelfUpdate::Verifying || s == SelfUpdate::Replacing || s == SelfUpdate::Restarting;
    const bool offered = s == SelfUpdate::Available;
    const bool failed = s == SelfUpdate::Failed;
    QString text = su->statusText();
    LauncherTheme::Role tone = LauncherTheme::Text;
    if (failed) tone = LauncherTheme::Error;
    // after an update the new launcher says so (once: until Not now, or the next start)
    if (!su->finishedText().isEmpty() && !offered && !busy && !failed) {
        text = su->finishedText();
        tone = su->updatedOk() ? LauncherTheme::Ok : LauncherTheme::Warn;
    }
    if (place_ == OnAbout && text.isEmpty())
        text = QStringLiteral("The launcher looks for a newer version of itself each time it opens.");
    line_->setText(text);
    line_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(tone)));
    progress_->setVisible(busy);
    if (busy) {
        progress_->setText(su->statusText());
        if (s != SelfUpdate::Downloading) progress_->setIndeterminate(true);
    } else {
        progress_->clear();
    }
    const QString tag = su->release().tag;
    update_->setText(QStringLiteral("Update the launcher to %1").arg(tag));
    update_->setAccessibleName(update_->text());
    // (a local, not update_->isVisible(): that is false while the box itself is hidden or its page not shown)
    const bool canUpdate = offered && blocked != SelfUpdate::NotInstalled && blocked != SelfUpdate::NotWritable &&
                           blocked != SelfUpdate::DiskImage && blocked != SelfUpdate::Translocated;
    update_->setVisible(canUpdate);
    update_->setEnabled(blocked == SelfUpdate::NoProblem);   // the game running: shown, not pressable, the line says why
    retry_->setVisible(failed && !tag.isEmpty() && su->problem() != SelfUpdate::NotInstalled && su->problem() != SelfUpdate::NotWritable &&
                       su->problem() != SelfUpdate::DiskImage && su->problem() != SelfUpdate::Translocated);
    download_->setVisible((offered && !canUpdate) || (failed && !tag.isEmpty()));
    check_->setVisible(place_ == OnAbout && !busy && !offered);
    later_->setVisible(place_ == OnGames && !busy && (offered || failed || !su->finishedText().isEmpty()));
    if (place_ == OnGames) {
        const bool finishedLine = !su->finishedText().isEmpty() && !finishedSeen_ && !offered && !busy && !failed;
        setVisible(busy || ((offered || failed) && !su->dismissed()) || finishedLine);
    }
}

}  // namespace ps2r::launcher
