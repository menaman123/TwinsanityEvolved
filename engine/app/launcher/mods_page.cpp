// app/launcher/mods_page.cpp -- the Mods page.
#include "mods_page.h"
#include <QDir>
#include <QDrag>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QScrollArea>
#include <QStandardPaths>
#include <QTimer>
#include <QVBoxLayout>
#include "app_data.h"
#include "empty_state.h"
#include "flat_button.h"
#include "folder_row.h"
#include "launcher_app.h"
#include "launcher_theme.h"
#include "mod_importer.h"
#include "mod_row.h"
#include "page_header.h"
#include "progress_panel.h"
#include "sheet_overlay.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

namespace {
const char kModMime[] = "application/x-twinsanityevolved-mod";   // lint: not-ui (an internal drag's data type)

bool copyTree(const QString& from, const QString& to) {
    if (!QDir().mkpath(to)) return false;
    for (const QFileInfo& fi : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString dest = to + "/" + fi.fileName();
        if (fi.isDir() && !fi.isSymLink()) { if (!copyTree(fi.absoluteFilePath(), dest)) return false; continue; }
        QFile::remove(dest);
        if (!QFile::copy(fi.absoluteFilePath(), dest)) return false;
    }
    return true;
}

qint64 folderBytes(const QString& dir) {
    qint64 total = 0;
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        if (fi.isDir() && !fi.isSymLink()) total += folderBytes(fi.absoluteFilePath());
        else total += fi.size();
    }
    return total;
}

QString sizeText(qint64 bytes) {
    if (bytes < 1024) return QStringLiteral("%1 bytes").arg(bytes);
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg((bytes + 512) / 1024);
    if (bytes < 1024ll * 1024 * 1024) return QStringLiteral("%1 MB").arg((bytes + 512 * 1024) / (1024 * 1024));
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
}

QLabel* groupTitle(const QString& text, QWidget* parent) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* l = new QLabel(text, parent);
    l->setFont(t.font(LauncherTheme::Title));
    l->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    return l;
}

QLabel* captionLabel(QWidget* parent) {
    const LauncherTheme& t = LauncherTheme::instance();
    auto* l = new QLabel(parent);
    l->setFont(t.font(LauncherTheme::Caption));
    l->setWordWrap(true);
    l->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
    return l;
}

// The library's stage as the progress caption: "Reading the patch · 40 %"
QString stageText(const QString& stage, double fraction) {
    QString s = stage.trimmed();
    if (!s.isEmpty()) s[0] = s[0].toUpper();
    if (fraction > 0.0 && fraction <= 1.0) s += QStringLiteral("%1%2 %").arg(s.isEmpty() ? QString() : QStringLiteral(" · ")).arg(int(fraction * 100.0 + 0.5));
    return s;
}

QString sentence(QString s) {
    s = s.trimmed();
    if (s.isEmpty()) return s;
    s[0] = s[0].toUpper();
    if (!s.endsWith('.') && !s.endsWith('!') && !s.endsWith('?')) s += '.';
    return s;
}
}  // namespace

ModsPage::ModsPage(LauncherApp* app, QWidget* parent) : QWidget(parent), app_(app), importer_(new ModImporter(this)) {
    const LauncherTheme& t = LauncherTheme::instance();
    setAcceptDrops(true);
    auto* scroll = new QScrollArea(this);
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* inner = new QWidget();
    scroll->setWidget(inner);
    auto* col = new QVBoxLayout(inner);
    col->setContentsMargins(LauncherTheme::S5, 0, LauncherTheme::S5, LauncherTheme::S5);
    col->setSpacing(LauncherTheme::S2);
    col->addWidget(new PageHeader(QStringLiteral("Mods"),
                                  QStringLiteral("Mods change what the game reads from your disc, never the disc image. While a mod is on, the "
                                                 "game saves to its own memory card. Changes apply at the next start."), inner));
    notSetUp_ = new EmptyState(QStringLiteral("Set up the game first"),
                               QStringLiteral("Mods live in the game folder, which the setup makes."), QStringLiteral("Set up…"), QString(), inner);
    notSetUp_->hide();
    col->addWidget(notSetUp_);
    notice_ = new QLabel(inner);
    notice_->setObjectName(QStringLiteral("mods_notice"));
    notice_->setFont(t.font(LauncherTheme::Body));
    notice_->setWordWrap(true);
    notice_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    notice_->hide();
    col->addWidget(notice_);
    // the import in progress: what is being added, the bar, the library's stage, Cancel
    importBox_ = new QWidget(inner);
    importBox_->setObjectName(QStringLiteral("mods_importing"));
    auto* importRow = new QHBoxLayout(importBox_);
    importRow->setContentsMargins(0, LauncherTheme::S1, 0, LauncherTheme::S1);
    importRow->setSpacing(LauncherTheme::S3);
    progress_ = new ProgressPanel(importBox_);
    cancel_ = new FlatButton(QStringLiteral("Cancel"), FlatButton::Secondary, importBox_);
    importRow->addWidget(progress_, 1);
    importRow->addWidget(cancel_, 0, Qt::AlignVCenter);
    importBox_->hide();
    col->addWidget(importBox_);
    builtins_ = new QWidget(inner);
    auto* bcol = new QVBoxLayout(builtins_);
    bcol->setContentsMargins(0, 0, 0, 0);
    bcol->setSpacing(0);
    bcol->addWidget(groupTitle(QStringLiteral("Included with the game"), builtins_));
    builtinsLayout_ = new QVBoxLayout();
    builtinsLayout_->setSpacing(0);
    bcol->addLayout(builtinsLayout_);
    col->addWidget(builtins_);
    for (const BuiltinMod& m : app->manifest()->builtinMods()) {
        ModRow* r = addRow(m.id, m.name, builtins_, builtinsLayout_);
        r->setByline(ModList::byline(m.author, QString()));
        r->setSwitchable(m.hasSwitch);
        r->setSummary(m.hasSwitch ? m.help : (QStringLiteral("Always in the game under Options > Mods. ") + m.help).trimmed());
    }
    col->addSpacing(LauncherTheme::S3);
    folderGroup_ = new QWidget(inner);
    auto* fcol = new QVBoxLayout(folderGroup_);
    fcol->setContentsMargins(0, 0, 0, 0);
    fcol->setSpacing(LauncherTheme::S1);
    fcol->addWidget(groupTitle(QStringLiteral("In your mods folder"), folderGroup_));
    auto* whereRow = new QHBoxLayout();
    whereRow->setSpacing(LauncherTheme::S2);
    where_ = captionLabel(folderGroup_);
    where_->setObjectName(QStringLiteral("mods_folder"));
    openMods_ = new FlatButton(QStringLiteral("Open the mods folder"), FlatButton::Quiet, folderGroup_);
    whereRow->addWidget(where_, 1);
    whereRow->addWidget(openMods_);
    fcol->addLayout(whereRow);
    orderNote_ = captionLabel(folderGroup_);
    orderNote_->setObjectName(QStringLiteral("mods_order_note"));
    orderNote_->setText(QStringLiteral("The lower mod wins where two change the same file. Drag a mod by its handle, or use Move up and Move down."));
    fcol->addWidget(orderNote_);
    folderHost_ = new QWidget(folderGroup_);
    folderLayout_ = new QVBoxLayout(folderHost_);
    folderLayout_->setContentsMargins(0, 0, 0, 0);
    folderLayout_->setSpacing(0);
    fcol->addWidget(folderHost_);
    dropLine_ = new QWidget(folderHost_);
    dropLine_->setStyleSheet(QStringLiteral("background: %1;").arg(t.css(LauncherTheme::Accent)));
    dropLine_->hide();
    col->addWidget(folderGroup_);
    empty_ = new EmptyState(QStringLiteral("Drop a mod here"),
                            QStringLiteral("A mod crate, a patched disc image, a patch file or a folder of files; nothing is downloaded."),
                            QStringLiteral("Add a mod…"), QStringLiteral("Add a folder…"), inner);
    empty_->setObjectName(QStringLiteral("mods_empty"));
    col->addWidget(empty_);
    auto* bottom = new QHBoxLayout();
    bottom->setSpacing(LauncherTheme::S2);
    add_ = new FlatButton(QStringLiteral("Add a mod…"), FlatButton::Secondary, inner);
    addFolder_ = new FlatButton(QStringLiteral("Add a folder…"), FlatButton::Quiet, inner);
    bottom->addWidget(add_);
    bottom->addWidget(addFolder_);
    bottom->addStretch(1);
    col->addSpacing(LauncherTheme::S2);
    col->addLayout(bottom);
    // the PS Vita at the foot: mods are switched here and copied, with the button right there
    if (!app->manifest()->vitaLabel().isEmpty() && !app->manifest()->vitaModsNote().isEmpty()) {
        vitaPanel_ = new QWidget(inner);
        vitaPanel_->setObjectName(QStringLiteral("mods_vita"));
        auto* vcol = new QVBoxLayout(vitaPanel_);
        vcol->setContentsMargins(0, 0, 0, 0);
        vcol->setSpacing(LauncherTheme::S1);
        vcol->addWidget(groupTitle(app->manifest()->vitaLabel(), vitaPanel_));
        vitaText_ = new QLabel(vitaPanel_);
        vitaText_->setObjectName(QStringLiteral("mods_vita_text"));
        vitaText_->setFont(t.font(LauncherTheme::Body));
        vitaText_->setWordWrap(true);
        // (no maximum width: the layout measures a word-wrapped label's height at the column's width)
        vitaText_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::TextMuted)));
        vcol->addWidget(vitaText_);
        vitaButton_ = new FlatButton(QString(), FlatButton::Secondary, vitaPanel_);
        vitaButton_->setObjectName(QStringLiteral("mods_vita_button"));
        vcol->addSpacing(LauncherTheme::S1);
        vcol->addWidget(vitaButton_, 0, Qt::AlignLeft);
        vitaLine_ = captionLabel(vitaPanel_);
        vitaLine_->setObjectName(QStringLiteral("mods_vita_line"));
        vitaLine_->hide();
        vcol->addWidget(vitaLine_);
        col->addSpacing(LauncherTheme::S4);
        col->addWidget(vitaPanel_);
        connect(vitaButton_, &QAbstractButton::clicked, this, [this] {
            if (app_->vitaFolder().isEmpty()) emit vitaSetupRequested();
            else makeVitaFolder();
        });
    }
    col->addStretch(1);
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->addWidget(scroll);

    auto pickFile = [this] {
        const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Choose a mod"), start,
                                                          QStringLiteral("Mods (*.zip *.iso *.bin *.xdelta *.vcdiff *.ppf *.bps);;All files (*)"));
        if (!path.isEmpty()) dropPath(path);
    };
    auto pickFolder = [this] {
        const QString start = QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
        const QString path = QFileDialog::getExistingDirectory(this, QStringLiteral("Choose a mod's folder"), start);
        if (!path.isEmpty()) dropPath(path);
    };
    connect(add_, &QAbstractButton::clicked, this, pickFile);
    connect(addFolder_, &QAbstractButton::clicked, this, pickFolder);
    connect(empty_, &EmptyState::primaryClicked, this, pickFile);
    connect(empty_, &EmptyState::quietClicked, this, pickFolder);
    connect(openMods_, &QAbstractButton::clicked, this, [this] {
        const QString dir = app_->modsFolder();
        QDir().mkpath(dir);
        FolderRow::revealInFileManager(dir);
    });
    connect(cancel_, &QAbstractButton::clicked, this, [this] {
        importer_->cancel();
        cancel_->setEnabled(false);
        progress_->setCaption(QStringLiteral("Stopping…"));
    });
    connect(importer_, &ModImporter::detected, this, [this](const QString& kind) {
        importKind_ = kind;
        progress_->setText(kind.isEmpty() ? QStringLiteral("Adding %1…").arg(importFile_) : QStringLiteral("Adding %1 (%2)…").arg(importFile_, kind));
    });
    connect(importer_, &ModImporter::progress, this, [this](const QString& stage, double fraction) {
        if (!cancel_->isEnabled()) return;   // stopping
        progress_->setCaption(stageText(stage, fraction));
        progress_->setProgress(int(qBound(0.0, fraction, 1.0) * 1000.0), 1000);
    });
    connect(importer_, &ModImporter::finished, this, &ModsPage::importFinished);
    connect(notSetUp_, &EmptyState::primaryClicked, this, &ModsPage::setupRequested);
    connect(app_, &LauncherApp::settingsChanged, this, [this](const QString& key) { if (key.isEmpty() || key == "paths.mods") reload(); });
    connect(app_, &LauncherApp::installsChanged, this, &ModsPage::reload);
    reload();
}

ModRow* ModsPage::addRow(const QString& id, const QString& name, QWidget* host, QVBoxLayout* layout) {
    auto* r = new ModRow(id, name, host);
    connect(r, &ModRow::switched, this, [this](const QString& mid, bool on) {
        QString err;
        if (!app_->setSettingValue("mods." + mid + ".enabled", on ? "on" : "off", &err)) {
            notice(QStringLiteral("This setting could not be saved: %1.").arg(err), true);
            if (ModRow* row = this->row(mid)) row->setOn(!on);
        }
        app_->dropLanguagePackKeys();
    });
    connect(r, &ModRow::moveRequested, this, [this](const QString& mid, int delta) { moveMod(mid, delta); });
    connect(r, &ModRow::openRequested, this, [this](const QString& mid) {
        for (const ModEntry& e : entries_) if (e.id == mid) FolderRow::revealInFileManager(e.dir);
    });
    connect(r, &ModRow::removeRequested, this, [this](const QString& mid) { openRemoveSheet(mid); });
    connect(r, &ModRow::dragRequested, this, &ModsPage::startDrag);
    rows_ << r;
    layout->addWidget(r);
    return r;
}

ModRow* ModsPage::row(const QString& id) const {
    for (ModRow* r : rows_) if (r->id() == id) return r;
    return nullptr;
}

QStringList ModsPage::folderOrder() const {
    QStringList ids;
    for (const ModEntry& e : entries_) if (e.readable) ids << e.id;
    return ids;
}

QString ModsPage::noticeText() const { return notice_->isVisible() ? notice_->text() : QString(); }

QString ModsPage::vitaPanelText() const { return vitaText_ && vitaPanel_->isVisible() ? vitaText_->text() : QString(); }
QString ModsPage::vitaLineText() const { return vitaLine_ && vitaLine_->isVisible() ? vitaLine_->text() : QString(); }

bool ModsPage::makeVitaFolder() {
    if (!vitaLine_) return false;
    VitaFolderReport r;
    const bool ok = app_->makeVitaFolder(&r);
    const TitleManifest* m = app_->manifest();
    vitaLine_->setText(ok ? QStringLiteral("Made again in %1: the settings, the disc's name (%2) and the mods. %3 Copy %4 to the %5 again.")
                                .arg(AppData::shortPath(r.folder), r.disc, VitaFolder::summary(r), m->vitaDataDir(), m->vitaLabel())
                          : QStringLiteral("The %1 folder was not made: %2").arg(m->vitaLabel(), r.error));
    vitaLine_->setStyleSheet(QStringLiteral("color: %1;").arg(LauncherTheme::instance().css(ok ? LauncherTheme::TextMuted : LauncherTheme::Error)));
    vitaLine_->show();
    return ok;
}

bool ModsPage::importing() const { return importBusy_; }

void ModsPage::notice(const QString& text, bool error) {
    const LauncherTheme& t = LauncherTheme::instance();
    noticeError_ = error;
    notice_->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(error ? LauncherTheme::Error : LauncherTheme::Ok)));
    notice_->setText(text);
    notice_->setVisible(!text.isEmpty());
}

void ModsPage::clearFolderRows() {
    for (int i = rows_.size() - 1; i >= 0; --i) {
        if (rows_[i]->parent() != folderHost_) continue;
        folderLayout_->removeWidget(rows_[i]);
        rows_[i]->hide();
        rows_[i]->deleteLater();
        rows_.remove(i);
    }
}

void ModsPage::reload() {
    const bool ready = !app_->gameFolder().isEmpty() && app_->setupDone();
    notSetUp_->setVisible(!ready);
    builtins_->setEnabled(ready);
    clearFolderRows();
    for (ModRow* r : rows_) r->setOn(app_->settingValue("mods." + r->id() + ".enabled") == QLatin1String("on"));
    const QString dir = ready ? app_->modsFolder() : QString();
    where_->setText(dir.isEmpty() ? QString() : AppData::shortPath(dir));
    QVector<ModEntry> found = ModList::mods(dir);   // a language pack is not a mod: never a row here
    for (ModEntry& e : found) {   // a built-in's id: the game keeps the built-in and skips the folder
        for (const BuiltinMod& b : app_->manifest()->builtinMods()) {
            if (e.readable && b.id == e.id) {
                e.readable = false;
                e.problem = QStringLiteral("A mod that comes with the game has this name already.");
            }
        }
    }
    entries_ = ModList::ordered(found, app_->settingValue(QStringLiteral("mods.order")));
    const QString discRegion = ready ? app_->importBaseRegion() : QString();
    const int movable = int(folderOrder().size());
    int at = 0;
    for (const ModEntry& e : entries_) {
        ModRow* r = addRow(e.id, e.name, folderHost_, folderLayout_);
        r->setFolderMod(true);
        if (!e.readable) {
            r->setBroken(e.problem);
            continue;
        }
        r->setByline(ModList::byline(e.author, e.version));
        r->setRegion(ModList::regionLabel(e.region));
        r->setSummary(ModList::summary(e, discRegion));
        r->setWarning(ModList::warningLines(e, discRegion));
        r->setNotes(ModList::notesText(e));
        r->setOn(app_->settingValue("mods." + e.id + ".enabled") == QLatin1String("on"));
        r->setMovable(at > 0, at < movable - 1);
        ++at;
    }
    folderGroup_->setVisible(ready);
    folderHost_->setVisible(!entries_.isEmpty());
    orderNote_->setVisible(movable > 1);
    empty_->setVisible(ready && entries_.isEmpty());
    if (vitaPanel_) {
        vitaPanel_->setVisible(ready);
        vitaText_->setText(VitaFolder::modsNote(app_->manifest(), app_->vitaConsoleDisc()));
        const bool haveVita = !app_->vitaFolder().isEmpty();
        vitaButton_->setText(haveVita ? QStringLiteral("Make the %1 folder again").arg(app_->manifest()->vitaLabel())
                                      : QStringLiteral("Get the game for %1…").arg(app_->manifest()->vitaLabel()));
        vitaButton_->setAccessibleName(vitaButton_->text());
        if (!haveVita) vitaLine_->hide();
    }
    add_->setVisible(ready && !entries_.isEmpty());
    addFolder_->setVisible(ready && !entries_.isEmpty());
    setBusy(importing());
}

void ModsPage::setBusy(bool busy) {
    importBox_->setVisible(busy);
    cancel_->setEnabled(busy);
    add_->setEnabled(!busy);
    addFolder_->setEnabled(!busy);
    empty_->primaryButton()->setEnabled(!busy);
    empty_->quietButton()->setEnabled(!busy);
}

bool ModsPage::moveModTo(const QString& id, int index) {
    QStringList ids = folderOrder();
    const int from = int(ids.indexOf(id));
    if (from < 0) return false;
    int to = qBound(0, index, int(ids.size()));
    if (to > from) --to;   // the slot counted the row itself
    if (to == from) return false;
    ids.removeAt(from);
    ids.insert(to, id);
    QString err;
    if (!app_->setSettingValue(QStringLiteral("mods.order"), ModList::orderValue(ids), &err)) {
        notice(QStringLiteral("The order could not be saved: %1.").arg(err), true);
        return false;
    }
    app_->dropLanguagePackKeys();
    reload();
    return true;
}

bool ModsPage::moveMod(const QString& id, int delta) {
    const QStringList ids = folderOrder();
    const int from = int(ids.indexOf(id));
    if (from < 0 || delta == 0) return false;
    const int to = qBound(0, from + delta, int(ids.size()) - 1);
    if (to == from) return false;
    return moveModTo(id, delta > 0 ? to + 1 : to);
}

bool ModsPage::readyFolderId(const QString& path, QString* id, QString* name, QString* message) {
    const QFileInfo fi(path);
    QFile f(path + "/mod.json");
    QJsonObject m;
    if (f.size() <= (1 << 20) && f.open(QIODevice::ReadOnly)) {
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
        if (doc.isObject()) m = doc.object();
    }
    if (m.isEmpty()) { *message = QStringLiteral("%1 has a mod.json that cannot be read.").arg(fi.fileName()); return false; }
    *id = m.value("id").toString();
    *name = m.value("name").toString(*id);
    const int schema = m.value("schema").toInt(1);
    const QString kind = m.value("kind").toString(schema == 1 ? QStringLiteral("builtin") : QString());
    if (!ModList::validId(*id)) {
        *message = QStringLiteral("%1 cannot be added: its description has no name the game can use (lower-case letters, digits and _).").arg(fi.fileName());
        return false;
    }
    if (schema != 1 && schema != 2) { *message = QStringLiteral("%1 is for a newer version of the game.").arg(fi.fileName()); return false; }
    if (kind != QLatin1String("disc") && kind != QLatin1String("language")) {
        *message = QStringLiteral("%1 cannot be added: the game does not read this kind of mod (%2).").arg(fi.fileName(), kind.isEmpty() ? QStringLiteral("none given") : kind);
        return false;
    }
    for (const BuiltinMod& b : app_->manifest()->builtinMods())
        if (b.id == *id) { *message = QStringLiteral("%1 cannot be added: a mod that comes with the game has this name already.").arg(fi.fileName()); return false; }
    const QString dest = app_->modsFolder() + "/" + *id;
    if (QFileInfo::exists(dest) && !QFileInfo::exists(dest + "/mod.json")) {
        *message = QStringLiteral("The mods folder has a folder named %1 that is not a mod: move it away, then add the mod again.").arg(*id);
        return false;
    }
    return true;
}

bool ModsPage::copyModFolder(const QString& path, bool replace, QString* message) {
    QString id, name;
    if (!readyFolderId(path, &id, &name, message)) return false;
    const QString mods = app_->modsFolder();
    const QString dest = mods + "/" + id;
    if (QFileInfo::exists(dest) && !replace) {   // the page asks before it replaces (dropPath)
        *message = QStringLiteral("A mod named %1 is already in the mods folder.").arg(id);
        return false;
    }
    // staged beside the mods (a hidden folder the game and the page skip), then moved into place; a mod of that id is moved
    // aside first and deleted once the new copy is there (a copy that fails leaves the old one as it was)
    const QString tmp = mods + "/.import-tmp";
    const QString staged = tmp + "/copy-" + id, old = tmp + "/old-" + id;
    auto cleanup = [&] {
        QDir(staged).removeRecursively();
        QDir(old).removeRecursively();
        QDir().rmdir(tmp);
    };
    cleanup();
    if (!QDir().mkpath(tmp) || !copyTree(path, staged)) {
        cleanup();
        *message = QStringLiteral("The mod could not be copied into the mods folder.");
        return false;
    }
    const bool had = QFileInfo::exists(dest);
    if (had && !QDir().rename(dest, old)) {
        cleanup();
        *message = QStringLiteral("%1 could not be replaced. If the game is open, close it and try again.").arg(AppData::shortPath(dest));
        return false;
    }
    if (!QDir().rename(staged, dest)) {
        if (had) QDir().rename(old, dest);
        cleanup();
        *message = QStringLiteral("The mod could not be copied into the mods folder.");
        return false;
    }
    cleanup();
    // a language pack is not a mod: it gets no row here; it is a choice of the Text language row
    const bool pack = ModList::isLanguagePack(ModList::readOne(dest));
    if (pack)
        *message = had ? QStringLiteral("%1 replaced the copy you had. It is a language for the game's text: choose it under Settings, Text language, or in the game's options.").arg(name)
                       : QStringLiteral("%1 was added. It is a language for the game's text: choose it under Settings, Text language, or in the game's options.").arg(name);
    else
        *message = had ? QStringLiteral("%1 replaced the copy you had; its switch and its place in the order are kept. It takes effect the next time the game starts.").arg(name)
                       : QStringLiteral("%1 was added. Switch it on below; it takes effect the next time the game starts.").arg(name);
    if (pack) app_->dropLanguagePackKeys();
    return true;
}

SheetOverlay* ModsPage::openReplaceSheet(const QString& id, const std::function<void()>& replace) {
    const LauncherTheme& t = LauncherTheme::instance();
    const ModEntry have = ModList::readOne(app_->modsFolder() + "/" + id);
    const QString what = have.version.trimmed().isEmpty() ? have.name : QStringLiteral("%1 %2").arg(have.name, have.version.trimmed());
    auto* content = new QWidget();
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    auto* text = new QLabel(QStringLiteral("The copy you have is removed first."), content);
    text->setObjectName(QStringLiteral("mods_replace_text"));
    text->setFont(t.font(LauncherTheme::Body));
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addWidget(text);
    if (replaceSheet_) { replaceSheet_->close(); replaceSheet_->deleteLater(); }
    auto* sheet = new SheetOverlay(QStringLiteral("Replace %1?").arg(what), content, window());
    sheet->setPanelWidth(560);
    auto* cancel = new FlatButton(QStringLiteral("Cancel"), FlatButton::Secondary);
    cancel->setObjectName(QStringLiteral("mods_replace_cancel"));
    auto* confirm = new FlatButton(QStringLiteral("Replace"), FlatButton::Primary);
    confirm->setObjectName(QStringLiteral("mods_replace_confirm"));
    sheet->addButton(cancel);
    sheet->addButton(confirm);
    sheet->setCancelButton(cancel);
    replaceSheet_ = sheet;
    auto done = [this, sheet] {
        sheet->close();
        sheet->deleteLater();
        if (replaceSheet_ == sheet) replaceSheet_ = nullptr;
    };
    connect(cancel, &QAbstractButton::clicked, this, [this, what, done] {
        done();
        notice(QStringLiteral("Nothing was added: you keep %1.").arg(what), false);
    });
    connect(confirm, &QAbstractButton::clicked, this, [replace, done] {
        done();
        replace();
    });
    sheet->open();
    return sheet;
}

void ModsPage::dropPath(const QString& path) {
    if (app_->gameFolder().isEmpty() || !app_->setupDone()) {
        notice(QStringLiteral("Set up the game first: mods live in the game folder."), true);
        return;
    }
    const QFileInfo fi(path);
    if (!fi.exists()) {
        notice(QStringLiteral("%1 cannot be found.").arg(fi.fileName()), true);
        return;
    }
    if (fi.isDir() && QFileInfo::exists(path + "/mod.json")) {
        QString id, name, message;
        if (!readyFolderId(path, &id, &name, &message)) {
            notice(message, true);
            return;
        }
        auto copy = [this, path](bool replace) {
            QString text;
            const bool ok = copyModFolder(path, replace, &text);
            notice(text, !ok);
            if (ok) reload();
        };
        if (QFileInfo::exists(app_->modsFolder() + "/" + id + "/mod.json")) {
            openReplaceSheet(id, [copy] { copy(true); });
            return;
        }
        copy(false);
        return;
    }
    requestImport(fi.absoluteFilePath());
}

void ModsPage::requestImport(const QString& path) {
    if (importing()) {
        notice(QStringLiteral("One mod is being added. Add the next one when it is done."), true);
        return;
    }
    if (app_->importBaseDisc().isEmpty()) {
        notice(QStringLiteral("Add your disc image on the Games page first: a mod is made against your disc."), true);
        return;
    }
    // the id the import will take (the library's own rule, read without importing): a mod of that id is replaced, asked first
    const QString id = QString::fromStdString(ModImporter::modIdFor(path));
    if (!id.isEmpty() && QFileInfo::exists(app_->modsFolder() + "/" + id + "/mod.json")) {
        openReplaceSheet(id, [this, path] { startImport(path); });
        return;
    }
    startImport(path);
}

void ModsPage::startImport(const QString& path) {
    if (importing()) {
        notice(QStringLiteral("One mod is being added. Add the next one when it is done."), true);
        return;
    }
    const QString base = app_->importBaseDisc();
    if (base.isEmpty()) {
        notice(QStringLiteral("Add your disc image on the Games page first: a mod is made against your disc."), true);
        return;
    }
    const QString mods = app_->modsFolder();
    QDir().mkpath(mods);
    importFile_ = QFileInfo(path).fileName();
    importKind_.clear();
    notice(QString(), false);
    progress_->setText(QStringLiteral("Adding %1…").arg(importFile_));
    progress_->setCaption(QStringLiteral("Starting"));
    progress_->setProgress(0, 0);
    if (!importer_->start(path, base, mods)) return;
    importBusy_ = true;
    setBusy(true);
}

void ModsPage::importFinished(const ImportOutcome& o) {
    importBusy_ = false;
    setBusy(false);
    const QString file = QFileInfo(o.artefact).fileName();
    if (o.ok) {
        QStringList lines;
        const QString name = o.name.isEmpty() ? o.id : o.name;
        lines << (o.replaced ? QStringLiteral("%1 replaced the copy you had; its switch and its place in the order are kept. It takes effect the next time the game starts.").arg(name)
                             : QStringLiteral("%1 was added. Switch it on below; it takes effect the next time the game starts.").arg(name));
        for (const QString& w : o.warnings) lines << sentence(w);
        notice(lines.join('\n'), false);
        reload();
        return;
    }
    if (o.cancelled) {
        notice(QStringLiteral("Adding %1 was stopped; nothing was added.").arg(file), false);
        return;
    }
    notice(QStringLiteral("%1 could not be added. %2").arg(file, sentence(o.error.isEmpty() ? QStringLiteral("The import did not finish") : o.error)), true);
}

SheetOverlay* ModsPage::openRemoveSheet(const QString& id) {
    const LauncherTheme& t = LauncherTheme::instance();
    const ModEntry* entry = nullptr;
    for (const ModEntry& e : entries_) if (e.id == id) entry = &e;
    if (!entry) return nullptr;
    const QString name = entry->name, dir = entry->dir;
    auto* content = new QWidget();
    auto* col = new QVBoxLayout(content);
    col->setContentsMargins(0, 0, 0, 0);
    col->setSpacing(LauncherTheme::S2);
    auto* text = new QLabel(QStringLiteral("This deletes the folder %1 (%2). Your disc image and your saves are not touched.")
                                .arg(AppData::shortPath(dir), sizeText(folderBytes(dir))), content);
    text->setObjectName(QStringLiteral("mods_remove_text"));
    text->setFont(t.font(LauncherTheme::Body));
    text->setWordWrap(true);
    text->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Text)));
    col->addWidget(text);
    auto* failed = new QLabel(content);
    failed->setObjectName(QStringLiteral("mods_remove_error"));
    failed->setFont(t.font(LauncherTheme::Body));
    failed->setWordWrap(true);
    failed->setStyleSheet(QStringLiteral("color: %1;").arg(t.css(LauncherTheme::Error)));
    failed->hide();
    col->addWidget(failed);
    if (removeSheet_) { removeSheet_->close(); removeSheet_->deleteLater(); }
    auto* sheet = new SheetOverlay(QStringLiteral("Remove %1?").arg(name), content, window());
    sheet->setPanelWidth(560);
    auto* cancel = new FlatButton(QStringLiteral("Cancel"), FlatButton::Secondary);
    cancel->setObjectName(QStringLiteral("mods_remove_cancel"));
    auto* remove = new FlatButton(QStringLiteral("Remove"), FlatButton::Primary);
    remove->setObjectName(QStringLiteral("mods_remove_confirm"));
    sheet->addButton(cancel);
    sheet->addButton(remove);
    sheet->setCancelButton(cancel);
    removeSheet_ = sheet;
    auto done = [this, sheet] {
        sheet->close();
        sheet->deleteLater();
        if (removeSheet_ == sheet) removeSheet_ = nullptr;
    };
    connect(cancel, &QAbstractButton::clicked, sheet, done);
    connect(remove, &QAbstractButton::clicked, this, [this, id, failed, done] {
        QString err;
        if (!removeMod(id, &err)) {   // the error state: what could not be removed, named; the sheet stays
            failed->setText(err);
            failed->show();
            return;
        }
        done();
    });
    sheet->open();
    return sheet;
}

bool ModsPage::removeMod(const QString& id, QString* error) {
    const QString mods = app_->modsFolder();
    const QString dir = QDir::cleanPath(mods + "/" + id);
    QString name = id;
    for (const ModEntry& e : entries_) if (e.id == id) name = e.name;
    // only a folder directly inside the mods folder, named `id`
    const QFileInfo fi(dir);
    if (id.isEmpty() || id.contains('/') || id.contains('\\') || id == "." || id == ".." ||
        QDir::cleanPath(fi.absolutePath()) != QDir::cleanPath(QFileInfo(mods).absoluteFilePath()) || !fi.exists()) {
        if (error) *error = QStringLiteral("%1 is not in the mods folder.").arg(name);
        return false;
    }
    const bool gone = fi.isSymLink() ? QFile::remove(dir) : QDir(dir).removeRecursively();
    if (!gone || QFileInfo::exists(dir)) {
        if (error) *error = QStringLiteral("%1 could not be removed. If the game is open, close it and try again.").arg(AppData::shortPath(dir));
        return false;
    }
    QString err;
    app_->removeSettingValue("mods." + id + ".enabled", &err);
    QStringList order = ModList::parseOrder(app_->settingValue(QStringLiteral("mods.order")));
    if (order.removeAll(id) > 0) {
        if (order.isEmpty()) app_->removeSettingValue(QStringLiteral("mods.order"), &err);
        else app_->setSettingValue(QStringLiteral("mods.order"), ModList::orderValue(order), &err);
    }
    app_->dropLanguagePackKeys();
    notice(QStringLiteral("%1 was removed.").arg(name), false);
    reload();
    return true;
}

void ModsPage::showImportingForScreenshot(const QString& file, const QString& stage, double fraction) {
    if (file.isEmpty()) { setBusy(importing()); return; }
    importFile_ = file;
    notice(QString(), false);
    progress_->setText(QStringLiteral("Adding %1…").arg(file));
    progress_->setCaption(stageText(stage, fraction));
    progress_->setProgress(int(fraction * 1000.0), 1000);
    setBusy(true);
}

void ModsPage::startDrag(const QString& id) {
    ModRow* r = row(id);
    if (!r || !folderOrder().contains(id)) return;
    auto* mime = new QMimeData();
    mime->setData(QString::fromLatin1(kModMime), id.toUtf8());
    auto* drag = new QDrag(this);   // the page's: the rows are rebuilt by the move
    drag->setMimeData(mime);
    const QPixmap picture = r->grab();
    drag->setPixmap(picture.scaledToWidth(qMin(picture.width(), 480), Qt::SmoothTransformation));
    drag->setHotSpot(QPoint(8, 12));
    dragging_ = true;
    pendingMoveId_.clear();
    drag->exec(Qt::MoveAction);
    dragging_ = false;
    showDropLine(-1);
    // the drop's move, now that the drag is over (the row that started it is still handling its mouse event)
    if (!pendingMoveId_.isEmpty()) {
        const QString moved = pendingMoveId_;
        pendingMoveId_.clear();
        moveModTo(moved, pendingMoveIndex_);
    }
}

int ModsPage::dropIndexAt(const QPoint& pagePos) const {
    const int y = folderHost_->mapFrom(this, pagePos).y();
    int index = 0;
    for (ModRow* r : rows_) {
        if (r->parent() != folderHost_ || r->broken()) continue;
        if (r->geometry().center().y() < y) ++index;
    }
    return index;
}

void ModsPage::showDropLine(int index) {
    if (index < 0) { dropLine_->hide(); return; }
    QVector<ModRow*> movable;
    for (ModRow* r : rows_) if (r->parent() == folderHost_ && !r->broken()) movable << r;
    if (movable.isEmpty()) { dropLine_->hide(); return; }
    const int y = index < movable.size() ? movable[index]->geometry().top() : movable.last()->geometry().bottom() + 1;
    dropLine_->setGeometry(0, qMax(0, y - 1), folderHost_->width(), 2);
    dropLine_->show();
    dropLine_->raise();
}

void ModsPage::showEvent(QShowEvent* event) {
    reload();
    QWidget::showEvent(event);
}

void ModsPage::dragEnterEvent(QDragEnterEvent* event) {
    if (event->mimeData()->hasFormat(QString::fromLatin1(kModMime)) || event->mimeData()->hasUrls()) event->acceptProposedAction();
}

void ModsPage::dragMoveEvent(QDragMoveEvent* event) {
    if (event->mimeData()->hasFormat(QString::fromLatin1(kModMime))) showDropLine(dropIndexAt(event->position().toPoint()));
    event->acceptProposedAction();
}

void ModsPage::dragLeaveEvent(QDragLeaveEvent*) { showDropLine(-1); }

void ModsPage::dropEvent(QDropEvent* event) {
    const QMimeData* mime = event->mimeData();
    if (mime->hasFormat(QString::fromLatin1(kModMime))) {
        const QString id = QString::fromUtf8(mime->data(QString::fromLatin1(kModMime)));
        const int index = dropIndexAt(event->position().toPoint());
        showDropLine(-1);
        event->acceptProposedAction();
        if (dragging_) {   // applied when the drag has ended (startDrag)
            pendingMoveId_ = id;
            pendingMoveIndex_ = index;
        } else {
            QTimer::singleShot(0, this, [this, id, index] { moveModTo(id, index); });
        }
        return;
    }
    for (const QUrl& u : mime->urls()) {
        if (u.isLocalFile()) { dropPath(u.toLocalFile()); break; }
    }
    event->acceptProposedAction();
}

}  // namespace ps2r::launcher
