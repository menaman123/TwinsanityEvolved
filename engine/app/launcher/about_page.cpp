// app/launcher/about_page.cpp -- the About page.
#include "about_page.h"
#include <QApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QEvent>
#include <QScrollArea>
#include <QUrl>
#include <QVBoxLayout>
#include "app_data.h"
#include "details_sheet.h"
#include "flat_button.h"
#include "folder_row.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "launcher_update_box.h"
#include "page_header.h"
#include "sheet_overlay.h"
#include "switch_control.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

namespace {
qint64 folderBytes(const QString& dir) {
    qint64 total = 0;
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (fi.isDir() && !fi.isSymLink()) total += folderBytes(fi.absoluteFilePath());
        else total += fi.size();
    }
    return total;
}

QString sizeText(qint64 bytes) {
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg((bytes + 512) / 1024);
    if (bytes < 1024ll * 1024 * 1024) return QStringLiteral("%1 MB").arg((bytes + 512 * 1024) / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
}
}  // namespace

AboutPage::AboutPage(LauncherApp* app, DetailsSheet* details, QWidget* parent) : QWidget(parent), app_(app), details_(details) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    scroll->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(LauncherTheme::S5, 0, LauncherTheme::S5, LauncherTheme::S5);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(new PageHeader(QStringLiteral("About"), QString(), inner));
    auto* product = new QLabel(app->manifest()->product(), inner);
    product->setFont(t.font(LauncherTheme::Title));
    product->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addWidget(product);
    auto* desc = new QLabel(app->manifest()->description(), inner);
    desc->setFont(t.font(LauncherTheme::Body));
    desc->setWordWrap(true);
    desc->setMaximumWidth(600);
    desc->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    col->addWidget(desc);
    col->addSpacing(LauncherTheme::S3);
    versions_ = new QLabel(inner);
    versions_->setObjectName(QStringLiteral("about_versions"));
    versions_->setFont(t.font(LauncherTheme::Body));
    versions_->setWordWrap(true);
    versions_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    versions_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    col->addWidget(versions_);
    // the launcher's own update: its state, Check for a newer launcher / Update the launcher to <tag>
    launcherUpdate_ = new LauncherUpdateBox(app, LauncherUpdateBox::OnAbout, inner);
    col->addWidget(launcherUpdate_);
    col->addSpacing(LauncherTheme::S3);
    places_ = new QWidget(inner);
    placesLayout_ = new QVBoxLayout(places_);
    placesLayout_->setContentsMargins(0, 0, 0, 0);
    placesLayout_->setSpacing(LauncherTheme::S1);
    col->addWidget(places_);
    col->addSpacing(LauncherTheme::S3);
    // the PS Vita, the whole flow (title.json targets.vita.about)
    if (!app->manifest()->vitaAbout().isEmpty()) {
        auto* vitaTitle = new QLabel(app->manifest()->vitaLabel(), inner);
        vitaTitle->setFont(t.font(LauncherTheme::Label));
        vitaTitle->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
        col->addWidget(vitaTitle);
        vita_ = new QLabel(inner);
        vita_->installEventFilter(this);   // kept as tall as its text at its width (eventFilter)
        vita_->setObjectName(QStringLiteral("about_vita"));
        vita_->setFont(t.font(LauncherTheme::Body));
        vita_->setWordWrap(true);
        vita_->setTextFormat(Qt::RichText);
        // (no maximum width: the column's layout measures a word-wrapped label's height at the column's width, so a
        // narrower label would lose its last line)
        vita_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        vita_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
        col->addWidget(vita_);
        col->addSpacing(LauncherTheme::S3);
    }
    auto* licTitle = new QLabel(QStringLiteral("Licences"), inner);
    licTitle->setFont(t.font(LauncherTheme::Label));
    licTitle->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addWidget(licTitle);
    auto* lic = new QHBoxLayout();
    lic->setSpacing(LauncherTheme::S2);
    auto* notices = new FlatButton(QStringLiteral("Third-party notices"), FlatButton::Quiet, inner);
    connect(notices, &QAbstractButton::clicked, this, [this] {
        const QString game = app_->gameFolder();
        for (const QString& p : {QApplication::applicationDirPath() + "/../Resources/THIRD_PARTY_NOTICES.md", QApplication::applicationDirPath() + "/THIRD_PARTY_NOTICES.md",
                                 game + "/THIRD_PARTY_NOTICES.md"}) {
            if (QFile::exists(p)) { details_->showLogFile(QStringLiteral("Third-party notices"), p); return; }
        }
        details_->showLog(QStringLiteral("Third-party notices"), QStringLiteral("The notices file is not beside this application."));
    });
    lic->addWidget(notices);
    for (const LicenceLink& l : app->manifest()->licences()) {
        auto* b = new FlatButton(l.label, FlatButton::Quiet, inner);
        connect(b, &QAbstractButton::clicked, this, [this, l] {
            for (const QString& p : {QApplication::applicationDirPath() + "/../Resources/" + l.path, QApplication::applicationDirPath() + "/" + l.path,
                                     app_->gameFolder() + "/" + l.path}) {
                if (QFile::exists(p)) { details_->showLogFile(l.label, p); return; }
            }
            details_->showLog(l.label, QStringLiteral("%1 comes with the game; it is in the game folder once the game is installed.").arg(l.path));
        });
        lic->addWidget(b);
    }
    if (!app->manifest()->pageUrl().isEmpty()) {
        auto* page = new FlatButton(QStringLiteral("Project page"), FlatButton::Quiet, inner);
        connect(page, &QAbstractButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl(app_->manifest()->pageUrl())); });
        lic->addWidget(page);
    }
    lic->addStretch(1);
    col->addLayout(lic);
    col->addSpacing(LauncherTheme::S4);
    auto* removeRow = new QHBoxLayout();
    auto* removeText = new QLabel(QStringLiteral("Remove the launcher's own data"), inner);
    removeText->setFont(t.font(LauncherTheme::Body));
    removeText->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    remove_ = new FlatButton(QStringLiteral("Remove…"), FlatButton::Secondary, inner);
    removeRow->addWidget(removeText, 1);
    removeRow->addWidget(remove_);
    col->addLayout(removeRow);
    col->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);
    connect(remove_, &QAbstractButton::clicked, this, &AboutPage::openRemoveSheet);
    connect(app_, &LauncherApp::installsChanged, this, &AboutPage::refresh);
    refresh();
}

QWidget* AboutPage::linkRow(const QString& label, const QString& path, QWidget* parent) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* w = new QWidget(parent);
    auto* row = new QHBoxLayout(w);
    row->setContentsMargins(0, 0, 0, 0);
    row->setSpacing(LauncherTheme::S2);
    auto* l = new QLabel(label, w);
    l->setFont(t.font(LauncherTheme::Label));
    l->setMinimumWidth(128);
    l->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    auto* p = new QLabel(AppData::shortPath(path), w);
    p->setFont(t.font(LauncherTheme::Body));
    p->setTextInteractionFlags(Qt::TextSelectableByMouse);
    p->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    auto* open = new FlatButton(QStringLiteral("Open"), FlatButton::Quiet, w);
    open->setAccessibleName(QStringLiteral("Open %1").arg(label));
    connect(open, &QAbstractButton::clicked, this, [path] { FolderRow::revealInFileManager(QFileInfo(path).isDir() ? path : QFileInfo(path).absolutePath()); });
    row->addWidget(l);
    row->addWidget(p, 1);
    row->addWidget(open);
    return w;
}

QString AboutPage::versionsText() const { return versions_->text(); }

QString AboutPage::vitaText() const {
    return vita_ ? VitaFolder::about(app_->manifest(), app_->vitaConsoleDisc()).join('\n') : QString();
}

void AboutPage::refresh() {
    QStringList lines;
    QString launcher = QStringLiteral("Launcher %1 · Qt %2").arg(app_->version(), QString::fromLatin1(qVersion()));
    lines << launcher;
    const QVector<InstallRecord> all = app_->installs();
    if (all.isEmpty()) lines << QStringLiteral("The game is not installed yet.");
    for (const InstallRecord& r : all)
        lines << QStringLiteral("Game %1 for %2, installed %3").arg(r.tag, app_->manifest()->targetLabel(r.target), r.at.date().toString(Qt::ISODate));
    versions_->setText(lines.join('\n'));
    while (QLayoutItem* item = placesLayout_->takeAt(0)) {
        if (item->widget()) item->widget()->deleteLater();
        delete item;
    }
    if (!app_->gameFolder().isEmpty()) {
        placesLayout_->addWidget(linkRow(QStringLiteral("Game folder"), app_->gameFolder(), places_));
        placesLayout_->addWidget(linkRow(QStringLiteral("Settings"), app_->settingsFilePath(), places_));
    }
    placesLayout_->addWidget(linkRow(QStringLiteral("Launcher data"), app_->appData()->root(), places_));
    placesLayout_->addWidget(linkRow(QStringLiteral("Logs"), app_->appData()->logsDir(), places_));
    if (vita_) {   // the disc's name on the console follows the disc the game starts with
        QString html = QStringLiteral("<ol style=\"margin-left: -24px;\">");
        for (const QString& line : VitaFolder::about(app_->manifest(), app_->vitaConsoleDisc())) html += QStringLiteral("<li>%1</li>").arg(line.toHtmlEscaped());
        vita_->setText(html + QStringLiteral("</ol>"));
    }
}

void AboutPage::showEvent(QShowEvent* event) {
    refresh();
    QWidget::showEvent(event);
}

// The PS Vita label kept as tall as its text at its width: the layout can measure a word-wrapped list's height short
// (Windows' fonts), which loses its last line
bool AboutPage::eventFilter(QObject* watched, QEvent* event) {
    if (watched == vita_ && event->type() == QEvent::Resize) {
        const int h = vita_->heightForWidth(vita_->width());
        if (h > 0 && vita_->minimumHeight() != h) vita_->setMinimumHeight(h);
    }
    return QWidget::eventFilter(watched, event);
}

void AboutPage::openRemoveSheet() {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* content = new QWidget();
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    const QString root = app_->appData()->root();
    auto* text = new QLabel(QStringLiteral("This deletes the launcher's own data in %1 (%2): its list of your discs, its logs and its "
                                           "window. Your disc images are not touched.").arg(AppData::shortPath(root), sizeText(folderBytes(root))), content);
    text->setFont(t.font(LauncherTheme::Body));
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addWidget(text);
    const QString gameDir = app_->gameFolder();
    auto* gameRow = new QHBoxLayout();
    auto* gameSwitch = new SwitchControl(content);
    gameSwitch->setAccessibleName(QStringLiteral("Also remove the game folder"));
    auto* gameText = new QLabel(gameDir.isEmpty() ? QStringLiteral("There is no game folder yet.")
                                                  : QStringLiteral("Also remove the game folder %1 (%2): the game, your settings, your saves and your mods.")
                                                        .arg(AppData::shortPath(gameDir), sizeText(folderBytes(gameDir))), content);
    gameText->setFont(t.font(LauncherTheme::Body));
    gameText->setWordWrap(true);
    gameText->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    gameSwitch->setEnabled(!gameDir.isEmpty());
    gameRow->addWidget(gameSwitch);
    gameRow->addWidget(gameText, 1);
    col->addLayout(gameRow);
    auto* sheet = new SheetOverlay(QStringLiteral("Remove"), content, window());
    auto* cancel = new FlatButton(QStringLiteral("Cancel"), FlatButton::Secondary);
    auto* remove = new FlatButton(QStringLiteral("Remove"), FlatButton::Primary);
    sheet->addButton(cancel);
    sheet->addButton(remove);
    sheet->setCancelButton(cancel);
    connect(cancel, &QAbstractButton::clicked, sheet, [sheet] { sheet->close(); sheet->deleteLater(); });
    auto* failed = new QLabel(content);
    failed->setObjectName(QStringLiteral("about_remove_error"));
    failed->setFont(t.font(LauncherTheme::Body));
    failed->setWordWrap(true);
    failed->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Error)));
    failed->hide();
    col->addWidget(failed);
    connect(remove, &QAbstractButton::clicked, this, [sheet, gameSwitch, gameDir, root, failed] {
        const QString home = AppData::homePath();
        QStringList left;
        if (gameSwitch->isChecked() && !gameDir.isEmpty() && gameDir != home && QFileInfo(gameDir).isDir() && !QDir(gameDir).removeRecursively())
            left << AppData::shortPath(gameDir);
        if (root.startsWith(home) && root != home && QFileInfo(root).isDir() && !QDir(root).removeRecursively()) left << AppData::shortPath(root);
        if (!left.isEmpty()) {   // the error state: what could not be removed, named; the sheet stays
            failed->setText(QStringLiteral("%1 could not be removed. If the game is open, close it and try again.").arg(left.join(QStringLiteral(" and "))));
            failed->show();
            return;
        }
        sheet->close();
        sheet->deleteLater();
        qApp->quit();
    });
    sheet->open();
}

}  // namespace ps2r::launcher
