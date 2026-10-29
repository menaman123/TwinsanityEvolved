// app/launcher/settings_page.cpp -- the Settings page.
#include "settings_page.h"
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QScrollArea>
#include <QVBoxLayout>
#include "app_data.h"
#include "disc_scanner.h"
#include "empty_state.h"
#include "flat_button.h"
#include "folder_row.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "page_header.h"
#include "settings_row.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

SettingsPage::SettingsPage(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    scroll->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(LauncherTheme::S5, 0, LauncherTheme::S5, LauncherTheme::S5);
    col->setSpacing(LauncherTheme::S1);
    col->addWidget(new PageHeader(QStringLiteral("Settings"), QStringLiteral("The same settings the game shows under Options; a change here is a change there."), inner));
    empty_ = new EmptyState(QStringLiteral("Set up the game first"),
                            QStringLiteral("The settings live in the game folder, which the setup makes."), QStringLiteral("Set up…"), QString(), inner);
    empty_->setObjectName(QStringLiteral("settings_empty"));
    col->addWidget(empty_);
    error_ = new QLabel(inner);
    error_->setObjectName(QStringLiteral("settings_error"));
    error_->setFont(t.font(LauncherTheme::Body));
    error_->setWordWrap(true);
    error_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Error)));
    error_->hide();
    col->addWidget(error_);
    body_ = new QWidget(inner);
    auto* bcol = new QVBoxLayout(body_);
    bcol->setContentsMargins(0, 0, 0, 0);
    bcol->setSpacing(LauncherTheme::S1);
    for (const SettingsGroupSpec& g : app->manifest()->settingsGroups()) {
        auto* title = new QLabel(g.title, body_);
        title->setFont(t.font(LauncherTheme::Title));
        title->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
        bcol->addSpacing(LauncherTheme::S3);
        bcol->addWidget(title);
        for (const SettingsRowSpec& spec : g.rows) {
            if (spec.kind == QLatin1String("folder") || spec.kind == QLatin1String("disc")) {
                auto* fr = new FolderRow(spec.label, body_);
                fr->setObjectName(QStringLiteral("setting_%1").arg(spec.key));
                fr->setToolTip(spec.help);
                const QString key = spec.key;
                if (spec.kind == QLatin1String("disc")) {
                    discKey_ = key;
                    connect(fr, &FolderRow::changeRequested, this, [this] {
                        QStringList filters;
                        for (const QString& e : app_->manifest()->discExtensions()) filters << "*" + e;
                        const QString start = QFileInfo(app_->discSetting()).absolutePath();
                        const QString iso = QFileDialog::getOpenFileName(this, QStringLiteral("Choose the disc the game starts with"), start,
                                                                         QStringLiteral("Disc images (%1)").arg(filters.join(' ')));
                        if (!iso.isEmpty()) chooseDisc(iso);
                    });
                } else {
                    connect(fr, &FolderRow::changeRequested, this, [this, key, label = spec.label] {
                        const QString dir = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose the %1 folder").arg(label.toLower()), app_->folderSetting(key));
                        if (!dir.isEmpty()) chooseFolder(key, dir);
                    });
                }
                folders_.insert(key, fr);
                bcol->addWidget(fr);
                if (!spec.help.isEmpty()) {
                    auto* help = new QLabel(spec.help, body_);
                    help->setFont(t.font(LauncherTheme::Caption));
                    help->setWordWrap(true);
                    help->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
                    bcol->addWidget(help);
                }
                continue;
            }
            auto* row = new SettingsRow(spec, body_);
            connect(row, &SettingsRow::valueChanged, this, [this, derived = !spec.choicesFrom.isEmpty()](const QString& key, const QString& value) {
                QString err;
                if (!app_->setSettingValue(key, value, &err)) {
                    showError(QStringLiteral("This setting could not be saved: %1.").arg(err));
                    reload();
                    return;
                }
                if (derived) {
                    // the two language rows name both from now on, as the game's menu does (the legacy key goes), and a
                    // language pack's switch from an earlier version goes with it (a language pack is not a mod)
                    for (SettingsRow* r : rows_)
                        if (!r->spec().choicesFrom.isEmpty() && r->spec().key != key) app_->setSettingValue(r->spec().key, r->value(), &err);
                    app_->removeSettingValue(QStringLiteral("game.language"), &err);
                    app_->dropLanguagePackKeys();
                }
                error_->hide();
                applyDependencies();
            });
            rows_ << row;
            bcol->addWidget(row);
        }
        if (g.target == QLatin1String("vita") && !vitaButton_) {
            // the PS Vita folder made again from this game folder: shown while there is a PS Vita install
            vitaButton_ = new FlatButton(QStringLiteral("Make the %1 folder again").arg(app->manifest()->vitaLabel()), FlatButton::Secondary, body_);
            vitaButton_->setObjectName(QStringLiteral("settings_vita_folder"));
            vitaButton_->setAccessibleName(vitaButton_->text());
            vitaLine_ = new QLabel(body_);
            vitaLine_->setObjectName(QStringLiteral("settings_vita_folder_line"));
            vitaLine_->setFont(t.font(LauncherTheme::Caption));
            vitaLine_->setWordWrap(true);
            vitaLine_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
            bcol->addSpacing(LauncherTheme::S1);
            bcol->addWidget(vitaButton_, 0, Qt::AlignLeft);
            bcol->addWidget(vitaLine_);
            connect(vitaButton_, &QAbstractButton::clicked, this, [this] { makeVitaFolder(); });
        }
    }
    note_ = new QLabel(body_);
    note_->setFont(t.font(LauncherTheme::Caption));
    note_->setWordWrap(true);
    note_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    bcol->addSpacing(LauncherTheme::S3);
    bcol->addWidget(note_);
    col->addWidget(body_);
    col->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    connect(empty_, &EmptyState::primaryClicked, this, &SettingsPage::setupRequested);
    connect(app_, &LauncherApp::installsChanged, this, &SettingsPage::reload);
    connect(app_->scanner(), &DiscScanner::found, this, [this](const DiscFound& d) {
        if (d.path != pendingDisc_) return;
        pendingDisc_.clear();
        QString err;
        if (!app_->setDiscSetting(d.path, &err)) showError(QStringLiteral("This setting could not be saved: %1.").arg(err));
        else error_->hide();
        reload();
    });
    connect(app_, &LauncherApp::discRefused, this, [this](const QString& path, const QString& reason) {
        if (path != pendingDisc_) return;
        pendingDisc_.clear();
        showError(QStringLiteral("%1 cannot be used. %2").arg(QFileInfo(path).fileName(), reason));
    });
    reload();
}

SettingsRow* SettingsPage::row(const QString& key) const {
    for (SettingsRow* r : rows_) if (r->spec().key == key) return r;
    return nullptr;
}

QString SettingsPage::errorText() const { return error_->isVisible() ? error_->text() : QString(); }

void SettingsPage::showError(const QString& text) {
    error_->setText(text);
    error_->show();
}

bool SettingsPage::chooseFolder(const QString& key, const QString& dir) {
    QString err;
    if (!app_->setFolderSetting(key, dir, &err)) {
        showError(QStringLiteral("This setting could not be saved: %1.").arg(err));
        return false;
    }
    error_->hide();
    reload();
    return true;
}

void SettingsPage::chooseDisc(const QString& iso) {
    pendingDisc_ = QFileInfo(iso).absoluteFilePath();
    app_->identify(pendingDisc_);   // written once it is known to be the game
}

QString SettingsPage::vitaFolderLine() const { return vitaLine_ && vitaLine_->isVisible() ? vitaLine_->text() : QString(); }

bool SettingsPage::makeVitaFolder() {
    if (!vitaLine_) return false;
    VitaFolderReport r;
    const bool ok = app_->makeVitaFolder(&r);
    const TitleManifest* m = app_->manifest();
    vitaLine_->setText(ok ? QStringLiteral("Made again in %1: the settings, the disc's name (%2) and the mods. %3 Copy %4 to the %5 again.")
                                .arg(AppData::shortPath(r.folder), r.disc, VitaFolder::summary(r), m->vitaDataDir(), m->vitaLabel())
                          : QStringLiteral("The %1 folder was not made: %2").arg(m->vitaLabel(), r.error));
    vitaLine_->show();
    return ok;
}

void SettingsPage::reload() {
    const bool ready = !app_->settingsFilePath().isEmpty();
    empty_->setVisible(!ready);
    body_->setVisible(ready);
    if (vitaButton_) {
        const QString folder = app_->vitaFolder();
        vitaButton_->setVisible(!folder.isEmpty());
        vitaLine_->setVisible(!folder.isEmpty());
        if (!folder.isEmpty())
            vitaLine_->setText(QStringLiteral("The %1 folder is %2: make it again after you import a mod or change a setting, then copy %3 to the %1 again.")
                                   .arg(app_->manifest()->vitaLabel(), AppData::shortPath(folder), app_->manifest()->vitaDataDir()));
    }
    if (!ready) return;
    const LauncherApp::LanguageChoices languages = app_->languageChoices();
    for (SettingsRow* r : rows_) {
        const QString from = r->spec().choicesFrom;
        if (from.isEmpty()) {
            r->setValue(app_->settingValue(r->spec().key));
            continue;
        }
        // a language row: the languages the game offers for the disc it starts with
        QVector<SettingsChoice> choices;
        for (const QString& name : from == QLatin1String("speech_languages") ? languages.speech : languages.text) {
            QString label = name;
            if (!label.isEmpty()) label[0] = label[0].toUpper();
            choices << SettingsChoice{name, label};
        }
        r->setChoices(choices);
        r->setValue(app_->languageValue(r->spec().key));
        r->setHelpOverride(languages.disc.isEmpty() ? QStringLiteral("The languages of your disc show here once the game has a disc: choose it under Disc below.")
                                                    : QString());
    }
    for (auto it = folders_.begin(); it != folders_.end(); ++it) {
        if (it.key() == discKey_) {
            const QString disc = app_->discSetting();
            it.value()->setPath(disc.isEmpty() ? QStringLiteral("none yet") : disc);
        } else {
            it.value()->setPath(app_->folderSetting(it.key()));
        }
    }
    const QString n = app_->manifest()->settingsNote();
    note_->setText((n.isEmpty() ? QString() : n + " ") + AppData::shortPath(app_->settingsFilePath()));
    applyDependencies();
}

void SettingsPage::applyDependencies() {
    // the game's rule: UNCAPPED forces vertical sync off for the session, and the vsync row says so
    SettingsRow* rate = row("video.frame_rate");
    SettingsRow* vsync = row("video.vsync");
    if (!rate || !vsync) return;
    const bool uncapped = rate->value() == "uncapped";
    vsync->setLocked(uncapped);
    vsync->setHelpOverride(uncapped ? QStringLiteral("Off while the frame rate is uncapped.") : QString());
}

void SettingsPage::showEvent(QShowEvent* event) {
    reload();
    QWidget::showEvent(event);
}

}  // namespace ps2r::launcher
