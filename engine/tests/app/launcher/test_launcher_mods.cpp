// tests/app/launcher/test_launcher_mods.cpp -- the Mods page against a mods folder (the launcher mods judge's driver), offscreen:
// the rows the page lists (the built-ins, then the folder's mods in the overlay order) with their lines (name, byline,
// region badge, summary, code-changes warning, switch), a switch writing mods.<id>.enabled, Move down and a drag's drop
// writing mods.order, Remove deleting a mod only after its confirmation (and dropping its keys), a dropped mod folder copied
// in, a dropped artefact handed to the import library (the stub refuses it with its sentence; the library adds the mod),
// an import cancelled at once (stopped, nothing added, nothing left behind), and an import without a disc refused. Every
// visible text is checked for developer vocabulary.
//
// On its own (ctest) it makes its own game folder, mods and disc and checks the exact lines. A judge gives
//   PS2R_MODS_MANIFEST  title.json            PS2R_MODS_FIXTURE  a folder of mod folders copied into the mods folder
//   PS2R_MODS_ISO       the disc imported against (never written)
//   PS2R_MODS_CRATE     the artefact dropped for the import       PS2R_MODS_REPORT  the JSON report written at the end
//   PS2R_MODS_USA       a mod folder made over the North American disc (copied in; its row on this game folder reported)
//   PS2R_MODS_SHOT      a folder: the listed page saved as mods-list.png (scrolled to PS2R_MODS_SHOT_ROW when given)
//   PS2R_MODS_SWITCH / PS2R_MODS_MOVE_DOWN / PS2R_MODS_DRAG (id:slot) / PS2R_MODS_REMOVE   the mods each step uses
// and judges the report itself (tools/judges/launcher_mods_judge.py).
#include <QtTest/QtTest>
#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QMimeData>
#include <QRegularExpression>
#include <QScrollArea>
#include <QTemporaryDir>
#include "app_data.h"
#include "empty_state.h"
#include "flat_button.h"
#include "game_folder.h"
#include "ini_settings.h"
#include "launcher_app.h"
#include "launcher_style.h"
#include "launcher_theme.h"
#include "main_window.h"
#include "mod_importer.h"
#include "mod_list.h"
#include "mod_row.h"
#include "mods_page.h"
#include "progress_panel.h"
#include "sheet_overlay.h"
#include "switch_control.h"
#include "title_manifest.h"
using namespace ps2r::launcher;

namespace {
void both32(QByteArray& b, int at, quint32 v) {
    for (int i = 0; i < 4; ++i) { b[at + i] = char(v >> (8 * i)); b[at + 7 - i] = char(v >> (8 * i)); }
}
void both16(QByteArray& b, int at, quint16 v) {
    b[at] = char(v); b[at + 1] = char(v >> 8); b[at + 2] = char(v >> 8); b[at + 3] = char(v);
}
QByteArray record(quint32 lba, quint32 size, bool dir, const QByteArray& id) {
    QByteArray r(33 + id.size() + ((33 + id.size()) & 1), '\0');
    r[0] = char(r.size());
    both32(r, 2, lba);
    both32(r, 10, size);
    r[25] = dir ? 2 : 0;
    both16(r, 28, 1);
    r[32] = char(id.size());
    r.replace(33, id.size(), id);
    return r;
}
// A tiny ISO 9660 volume (PVD at 16, root at 18, files from 20): the standalone run's disc
QByteArray makeIso(const QVector<QPair<QString, QByteArray>>& files) {
    quint32 next = 20;
    QVector<quint32> lbas;
    for (const auto& f : files) { lbas << next; next += quint32(qMax<qsizetype>(1, (f.second.size() + 2047) / 2048)); }
    QByteArray img(int(next) * 2048, '\0');
    QByteArray root = record(18, 2048, true, QByteArray(1, '\0')) + record(18, 2048, true, QByteArray(1, '\1'));
    for (int i = 0; i < files.size(); ++i) {
        root += record(lbas[i], quint32(files[i].second.size()), false, (files[i].first + ";1").toLatin1());
        img.replace(int(lbas[i]) * 2048, files[i].second.size(), files[i].second);
    }
    img.replace(18 * 2048, root.size(), root);
    QByteArray pvd(2048, '\0');
    pvd[0] = 1;
    pvd.replace(1, 5, "CD001");
    pvd[6] = 1;
    pvd.replace(40, 32, QByteArray("CRASH").leftJustified(32, ' '));
    both32(pvd, 80, next);
    both16(pvd, 128, 2048);
    const QByteArray rootRec = record(18, 2048, true, QByteArray(1, '\0'));
    pvd.replace(156, rootRec.size(), rootRec);
    img.replace(16 * 2048, 2048, pvd);
    QByteArray term(2048, '\0');
    term[0] = static_cast<char>(-1);   // 0xFF: the set terminator's type
    term.replace(1, 5, "CD001");
    img.replace(17 * 2048, 2048, term);
    return img;
}

bool writeFile(const QString& path, const QByteArray& data) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    return f.open(QIODevice::WriteOnly) && f.write(data) == data.size();
}

bool copyTree(const QString& from, const QString& to) {
    if (!QDir().mkpath(to)) return false;
    for (const QFileInfo& fi : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
        const QString dest = to + "/" + fi.fileName();
        if (fi.isDir() && !fi.isSymLink()) { if (!copyTree(fi.absoluteFilePath(), dest)) return false; continue; }
        if (!QFile::copy(fi.absoluteFilePath(), dest)) return false;
    }
    return true;
}

const char* kManifest = R"JSON({
  "schema": 2, "id": "SLES-52568", "name": "Crash Twinsanity", "product": "TwinsanityEvolved", "app_data_folder": "TwinsanityEvolved",
  "binary_name": "twinsanityevolved", "description": "Crash Twinsanity running natively on your computer.",
  "disc": { "manifests": [], "plays": ["SLES_525.68"], "extensions": [".iso"] },
  "release": { "releases_url": "http:\/\/127.0.0.1:9/releases" },
  "targets": { "computers": [ { "id": "macos-arm64", "label": "macOS (Apple silicon)", "exe": "twinsanityevolved" },
                              { "id": "macos-x86_64", "label": "macOS (Intel)", "exe": "twinsanityevolved" },
                              { "id": "linux-x86_64", "label": "Linux (x86-64)", "exe": "twinsanityevolved" },
                              { "id": "linux-aarch64", "label": "Linux (ARM)", "exe": "twinsanityevolved" },
                              { "id": "windows-x64", "label": "Windows", "exe": "twinsanityevolved.exe" } ] },
  "game_folder": { "default": "~/Games/Crash Twinsanity", "settings": "ps2r.ini", "memcards": "memcards", "mods": "mods", "disc_file": "disc-image.txt" },
  "settings": { "groups": [ { "title": "Folders", "rows": [
      { "key": "paths.memcards", "label": "Memory cards", "kind": "folder", "default": "memcards", "write_default": true },
      { "key": "paths.mods", "label": "Mods", "kind": "folder", "default": "mods", "write_default": true } ] } ] },
  "mods": { "builtin": [
      { "id": "level_select", "name": "Level select", "author": "TwinsanityEvolved", "switch": false, "help": "A list of every level." },
      { "id": "speedrun", "name": "Speedrun mode", "author": "TwinsanityEvolved", "help": "A timer at the top of the screen." } ] }
})JSON";

// The standalone run's mods (what a judge's fixture folder stands for)
void writeOwnMods(const QString& mods) {
    writeFile(mods + "/alpha/mod.json", R"J({"schema": 2, "id": "alpha", "kind": "disc", "name": "Alpha caves", "author": "A modder", "version": "1.2",
        "base": {"region": "PAL", "executable": "SLES_525.68"},
        "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges"}, {"path": "SLES_525.68", "form": "replace"}],
        "executable": {"data_changes": {".rodata": 4, ".data": 0}, "code_changes": []}})J");
    writeFile(mods + "/beta/mod.json", R"J({"schema": 2, "id": "beta", "kind": "disc", "name": "Beta text", "version": "3",
        "notes": ["Apply the settings first.", "Good luck."],
        "base": {"region": "NTSC-U"}, "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges", "bytes": 929914953}],
        "import": {"game_code_settings": ["Classic Slide Jump for Crash", "Classic Explosion Damage"]}})J");
    QDir().mkpath(mods + "/beta/disc");
    writeFile(mods + "/gamma/mod.json", R"J({"schema": 2, "id": "gamma", "kind": "disc", "name": "Gamma agony", "author": "Someone",
        "base": {"region": "any"}, "files": [],
        "executable": {"data_changes": {".text": 0}, "code_changes": ["FUN_001ac8d0 Font::ConstructBase", "FUN_001ac900 Font::Draw",
                       "FUN_00200000 Thing::One", "FUN_00200100 Thing::Two"]}})J");
    QDir().mkpath(mods + "/gamma/disc");
    writeFile(mods + "/lingo/mod.json", R"J({"schema": 2, "id": "lingo", "kind": "language", "name": "lingo (text)", "author": "Translators",
        "version": "1.0.0", "base": {"region": "PAL"}, "language": {"id": "lingo", "label": "lingo (text)", "text_bank": "data/x.bank"}})J");
    writeFile(mods + "/zbroken/mod.json", "{ this is not json");
    writeFile(mods + "/Bad Name/mod.json", R"J({"schema": 2, "id": "Bad Name", "kind": "disc", "name": "Bad name"})J");
    writeFile(mods + "/.hidden/mod.json", "{}");   // hidden: never a mod
}

const QRegularExpression kDeveloperWords("\\b(cmake|ninja|python|wasm\\w*|wasi\\w*|toolchain|vcpkg|clang\\w*|compiler|compile|build|nativeee|recomp\\w*|decomp\\w*|vitasdk|seam)\\b",
                                         QRegularExpression::CaseInsensitiveOption);

QJsonObject rowJson(const ModRow* r, const QString& group, int index) {
    QJsonObject o;
    o["id"] = r->id();
    o["name"] = r->nameText();
    o["byline"] = r->bylineText();
    o["region"] = r->regionText();
    o["summary"] = r->summaryText();
    o["warning"] = r->warningText();
    o["notes"] = r->notesText();
    o["switchable"] = r->switchable() && !r->broken();
    o["on"] = r->on();
    o["broken"] = r->broken();
    o["group"] = group;
    o["index"] = index;
    o["move_up"] = r->upButton()->isVisible() && r->upButton()->isEnabled();
    o["move_down"] = r->downButton()->isVisible() && r->downButton()->isEnabled();
    o["open_folder"] = r->openButton()->isVisible();
    o["remove"] = r->removeButton()->isVisible();
    return o;
}
// A ready mod folder's id (its mod.json)
bool readyId(const QString& dir, QString* id) {
    QFile f(dir + "/mod.json");
    if (!f.open(QIODevice::ReadOnly)) return false;
    *id = QJsonDocument::fromJson(f.readAll()).object().value("id").toString();
    return !id->isEmpty();
}

// The replace sheet's buttons, sentence and heading (the page's own, by objectName and text)
FlatButton* sheetButton(SheetOverlay* sheet, const char* name) { return sheet ? sheet->findChild<FlatButton*>(name) : nullptr; }
QString sheetText(SheetOverlay* sheet) {
    QLabel* l = sheet ? sheet->findChild<QLabel*>("mods_replace_text") : nullptr;
    return l ? l->text() : QString();
}
QString sheetTitle(SheetOverlay* sheet) {
    QString t;
    if (sheet)
        for (QLabel* l : sheet->findChildren<QLabel*>())
            if (l->text().startsWith("Replace ")) t = l->text();
    return t;
}
}  // namespace

class TestLauncherMods : public QObject {
    Q_OBJECT
    QTemporaryDir home_;
    LauncherApp* app_ = nullptr;
    MainWindow* window_ = nullptr;
    QString folder_, mods_, crate_, iso_;
    bool own_ = true;   // no fixture from a judge: our own mods and exact lines
    QJsonObject report_;

    ModsPage* page() const { return window_->modsPage(); }
    IniSettings ini() const { return IniSettings(app_->settingsFilePath()); }
    QJsonArray rowsNow() const {
        QJsonArray rows;
        int b = 0, f = 0;
        for (ModRow* r : page()->rows()) {
            if (!r->isVisibleTo(page())) continue;
            rows << rowJson(r, r->folderMod() ? QStringLiteral("folder") : QStringLiteral("builtin"), r->folderMod() ? f++ : b++);
        }
        return rows;
    }
    QString env(const char* name, const QString& fallback) const {
        const QString v = qEnvironmentVariable(name);
        return v.isEmpty() ? fallback : v;
    }
    void visibleTextIsClean() {
        for (QLabel* l : window_->findChildren<QLabel*>()) {
            if (!l->isVisible()) continue;
            QVERIFY2(!kDeveloperWords.match(l->text()).hasMatch(), qPrintable("developer vocabulary in: " + l->text()));
        }
        for (FlatButton* b : window_->findChildren<FlatButton*>()) {
            if (!b->isVisible()) continue;
            QVERIFY2(!b->text().isEmpty(), "a visible button without text");
            QVERIFY2(!kDeveloperWords.match(b->text()).hasMatch(), qPrintable(b->text()));
        }
    }

private slots:
    void initTestCase() {
        qputenv("HOME", home_.path().toUtf8());
        qputenv("USERPROFILE", home_.path().toUtf8());
        qputenv("LOCALAPPDATA", home_.path().toUtf8());
        LauncherStyle::apply();
        LauncherTheme::instance().setDark(false);
        QString manifest = qEnvironmentVariable("PS2R_MODS_MANIFEST");
        if (manifest.isEmpty()) {
            manifest = home_.path() + "/titles/title.json";
            QVERIFY(writeFile(manifest, kManifest));
        }
        app_ = new LauncherApp(this);
        QString err;
        QVERIFY2(app_->loadManifest(manifest, &err), qPrintable(err));
        // the game folder as the setup makes it, recorded as this computer's install (no download)
        folder_ = GameFolder::suggested(app_->manifest());
        app_->setChosenFolder(folder_);
        QVERIFY2(GameFolder(app_->manifest(), folder_).prepare(app_->settingDefaults(), &err), qPrintable(err));
        QJsonObject install{{"target", app_->hostTarget()}, {"folder", folder_}, {"tag", "v0.3.0"}, {"placed", folder_ + "/twinsanityevolved"},
                            {"at", QDateTime::currentDateTime().toString(Qt::ISODate)}};
        app_->appData()->setValue("installs", QJsonArray{install});
        QVERIFY(app_->appData()->save());
        QVERIFY(app_->setupDone());
        mods_ = app_->modsFolder();
        QCOMPARE(mods_, folder_ + "/mods");
        const QString fixture = qEnvironmentVariable("PS2R_MODS_FIXTURE");
        own_ = fixture.isEmpty();
        if (own_) writeOwnMods(mods_);
        else QVERIFY2(copyTree(fixture, mods_), qPrintable("cannot copy " + fixture));
        // the disc a mod is imported against
        iso_ = qEnvironmentVariable("PS2R_MODS_ISO");
        if (iso_.isEmpty()) {
            iso_ = home_.path() + "/Downloads/Crash Twinsanity.iso";
            QVERIFY(writeFile(iso_, makeIso({{"SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.01\r\nVMODE = PAL\r\n"}, {"SLES_525.68", QByteArray(4096, 'x')}})));
        }
        QVERIFY(app_->setDiscSetting(iso_, &err));
        QCOMPARE(app_->importBaseDisc(), QFileInfo(iso_).absoluteFilePath());
        // the artefact dropped for the import: a Mod Crate folder (its info file and one layer0 file)
        crate_ = qEnvironmentVariable("PS2R_MODS_CRATE");
        if (crate_.isEmpty()) {
            crate_ = home_.path() + "/Downloads/Cave Remix";
            QVERIFY(writeFile(crate_ + "/ModCrateInfo.txt", "Name=Cave Remix\nAuthor=A modder\nVersion=2.0\nDescription=Hot.\n"));
            QVERIFY(writeFile(crate_ + "/layer0/SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 1.02\r\nVMODE = PAL\r\n"));
        }
#ifdef PS2R_LAUNCHER_IMPORT_library
        report_["import_kind"] = "library";
#else
        report_["import_kind"] = "stub";
#endif
        report_["mods_folder"] = mods_;
        report_["settings_file"] = app_->settingsFilePath();
        window_ = new MainWindow(app_);
        window_->resize(1120, 720);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_));
        window_->showPage(MainWindow::ModsIndex);
        QTest::qWait(100);
    }

    void cleanupTestCase() {
        const QString path = qEnvironmentVariable("PS2R_MODS_REPORT");
        if (!path.isEmpty()) {
            QFile ini(app_->settingsFilePath());
            if (ini.open(QIODevice::ReadOnly)) report_["settings_text"] = QString::fromUtf8(ini.readAll());
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(report_).toJson(QJsonDocument::Indented));
        }
        delete window_;
    }

    void listsTheFolder() {
        ModsPage* m = page();
        QVERIFY(!m->emptyState()->isVisible());
        const QJsonArray rows = rowsNow();
        report_["rows"] = rows;
        report_["folder_order_listed"] = QJsonArray::fromStringList(m->folderOrder());
        // the built-ins first: level select without a switch, speedrun mode with one
        QVERIFY(m->row("level_select"));
        QVERIFY(!m->row("level_select")->switchable());
        QVERIFY(m->row("level_select")->summaryText().startsWith("Always in the game under Options > Mods."));
        QVERIFY(m->row("speedrun") && m->row("speedrun")->switchable());
        // the folder's mods by id (no mods.order yet), the unreadable ones last
        QStringList ids = m->folderOrder();
        QStringList sorted = ids;
        sorted.sort();
        QCOMPARE(ids, sorted);
        QVERIFY(!m->row(".hidden") && !m->row(".import-tmp"));
        visibleTextIsClean();
        const QString shots = qEnvironmentVariable("PS2R_MODS_SHOT");
        if (!shots.isEmpty()) {   // a picture of the list as it is (evidence for a real mods folder)
            ModRow* r = m->row(qEnvironmentVariable("PS2R_MODS_SHOT_ROW"));
            if (QScrollArea* scroll = m->findChild<QScrollArea*>(); scroll && r) {
                QTest::qWait(50);
                scroll->ensureWidgetVisible(r, 0, 200);
                QTest::qWait(50);
            }
            QVERIFY(window_->grab().save(shots + "/mods-list.png"));
        }
        if (!own_) return;
        QCOMPARE(ids, QStringList({"alpha", "beta", "gamma"}));   // lingo is a language pack: not a mod, no row
        ModRow* a = m->row("alpha");
        QCOMPARE(a->nameText(), QStringLiteral("Alpha caves"));
        QCOMPARE(a->bylineText(), QStringLiteral("by A modder · 1.2"));
        QCOMPARE(a->regionText(), QStringLiteral("PAL"));
        QCOMPARE(a->summaryText(), QStringLiteral("Changes 2 files; data changes in the executable."));
        QCOMPARE(a->warningText(), QString());
        QVERIFY(!a->upButton()->isEnabled() && a->downButton()->isEnabled());
        ModRow* b = m->row("beta");
        QCOMPARE(b->bylineText(), QStringLiteral("3"));
        QCOMPARE(b->regionText(), QStringLiteral("NTSC-U"));
        // made over the North American disc, listed for a European one; its recipe records no base, so the game composes
        // it over this disc all the same: the summary names the disc it was made for, and no "Left out on this disc"
        QCOMPARE(b->summaryText(), QStringLiteral("Changes CRASH6/CRASH.BD, made for the North American disc."));
        QCOMPARE(b->warningText(), QStringLiteral("It asks for Crate Mod Loader settings the game cannot apply: Classic Slide Jump for Crash, "
                                                  "Classic Explosion Damage."));
        QCOMPARE(b->notesText(), QStringLiteral("Apply the settings first.\nGood luck."));
        ModRow* g = m->row("gamma");
        QCOMPARE(g->regionText(), QStringLiteral("Any region"));
        QCOMPARE(g->warningText(), QStringLiteral("Code changes not supported: FUN_001ac8d0 Font::ConstructBase, FUN_001ac900 Font::Draw, "
                                                  "FUN_00200000 Thing::One and 1 more."));
        QVERIFY(!m->row("lingo"));   // no row, no switch, no greyed-out line, no note: it is a choice of Settings, Text language
        QVERIFY(g->upButton()->isEnabled() && !g->downButton()->isEnabled());   // the last mod of the order is gamma
        QVERIFY(m->row("zbroken") && m->row("zbroken")->broken());
        QCOMPARE(m->row("zbroken")->summaryText(), QStringLiteral("This mod's description cannot be read."));
        QVERIFY(m->row("Bad Name") && m->row("Bad Name")->broken());
        QVERIFY(m->row("Bad Name")->removeButton()->isVisible());
    }

    void switchWritesTheKey() {
        const QString id = env("PS2R_MODS_SWITCH", "beta");
        ModRow* r = page()->row(id);
        QVERIFY2(r && r->switchable(), qPrintable(id));
        const QString key = "mods." + id + ".enabled";
        QJsonObject s{{"id", id}, {"before", ini().get(key)}};
        r->toggle()->click();
        s["after_on"] = ini().get(key);
        QCOMPARE(ini().get(key), QStringLiteral("on"));
        page()->reload();
        QVERIFY(page()->row(id)->on());
        page()->row(id)->toggle()->click();
        s["after_off"] = ini().get(key);
        QCOMPARE(ini().get(key), QStringLiteral("off"));
        page()->row(id)->toggle()->click();   // left on for the report (the judge reads the file)
        s["final"] = ini().get(key);
        // the built-in's switch writes its key the same way
        page()->row("speedrun")->toggle()->click();
        s["speedrun"] = ini().get("mods.speedrun.enabled");
        QCOMPARE(ini().get("mods.speedrun.enabled"), QStringLiteral("on"));
        page()->row("speedrun")->toggle()->click();
        report_["switch"] = s;
        // a language pack's switch and place from an earlier version (mods.<pack>.enabled, mods.order) go at the next write
        const QVector<ModEntry> packs = ModList::languagePacks(app_->modsFolder());
        if (!packs.isEmpty()) {
            const QString pack = packs.first().id;
            QVERIFY(ini().set("mods." + pack + ".enabled", "on"));
            QVERIFY(ini().set("mods.order", pack + "," + page()->folderOrder().join(',')));
            page()->row(id)->toggle()->click();
            page()->row(id)->toggle()->click();
            QVERIFY(!ini().has("mods." + pack + ".enabled"));
            QVERIFY(!ModList::parseOrder(ini().get("mods.order")).contains(pack));
            report_["stale_pack_keys_dropped"] = pack;
            QVERIFY(ini().remove("mods.order"));
        }
    }

    void orderIsWritten() {
        ModsPage* m = page();
        QJsonObject o;
        const QStringList before = m->folderOrder();
        o["before"] = QJsonArray::fromStringList(before);
        const QString down = env("PS2R_MODS_MOVE_DOWN", before.first());
        QVERIFY(m->row(down));
        m->row(down)->downButton()->click();   // Move down: the keyboard's way
        o["move_down"] = down;
        o["after_move_down"] = ini().get("mods.order");
        o["listed_after_move_down"] = QJsonArray::fromStringList(m->folderOrder());
        QStringList want = before;
        const int at = int(want.indexOf(down));
        want.move(at, at + 1);
        QCOMPARE(m->folderOrder(), want);
        QCOMPARE(ini().get("mods.order"), want.join(','));
        // a drag's drop: the mod into a slot (what dropEvent calls with the slot under the cursor)
        const QString drag = env("PS2R_MODS_DRAG", before.last() + ":0");
        const QString dragId = drag.section(':', 0, 0);
        const int slot = drag.section(':', 1, 1).toInt();
        QVERIFY(m->moveModTo(dragId, slot));
        o["drag"] = drag;
        o["after_drag"] = ini().get("mods.order");
        o["listed_after_drag"] = QJsonArray::fromStringList(m->folderOrder());
        QStringList want2 = want;
        const int from = int(want2.indexOf(dragId));
        want2.removeAt(from);
        want2.insert(slot > from ? slot - 1 : slot, dragId);
        QCOMPARE(m->folderOrder(), want2);
        QCOMPARE(ini().get("mods.order"), want2.join(','));
        // the page's own drop: a drop event carrying the row, released below the last row (the slot after it)
        {
            const QString dropped = want2.first();
            // the rows the last move rebuilt are shown and laid out by the event loop
            QVERIFY(QTest::qWaitFor([&] { ModRow* r = m->row(want2.last()); return r && r->isVisible() && r->height() > 0; }, 5000));
            QTest::qWait(50);
            ModRow* last = m->row(want2.last());
            const QPoint dropAt = last->mapTo(m, QPoint(20, last->height() - 2));
            auto* mime = new QMimeData();
            mime->setData(QStringLiteral("application/x-twinsanityevolved-mod"), dropped.toUtf8());
            QDragEnterEvent enter(dropAt, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(m, &enter);
            QDragMoveEvent move(dropAt, Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(m, &move);
            QDropEvent drop(QPointF(dropAt), Qt::MoveAction, mime, Qt::LeftButton, Qt::NoModifier);
            QApplication::sendEvent(m, &drop);
            QStringList want3 = want2;
            want3.removeAll(dropped);
            want3 << dropped;
            QVERIFY(QTest::qWaitFor([&] { return m->folderOrder() == want3; }, 5000));
            QCOMPARE(ini().get("mods.order"), want3.join(','));
            o["drop_event"] = dropped + ":end";
            o["after_drop_event"] = ini().get("mods.order");
            want2 = want3;
            delete mime;
        }
        // the ends: the first cannot go up, the last cannot go down
        QVERIFY(!m->row(want2.first())->upButton()->isEnabled());
        QVERIFY(!m->row(want2.last())->downButton()->isEnabled());
        QVERIFY(!m->moveMod(want2.first(), -1));
        report_["order"] = o;
    }

    void removeAsksFirst() {
        ModsPage* m = page();
        const QString id = env("PS2R_MODS_REMOVE", own_ ? QStringLiteral("gamma") : m->folderOrder().last());
        QVERIFY(m->row(id));
        const QString dir = mods_ + "/" + id;
        QJsonObject r{{"id", id}, {"dir", dir}};
        QVERIFY(QFileInfo(dir).isDir());
        m->row(id)->toggle()->click();   // on, and in the order: both must go with the mod
        QVERIFY(ini().get("mods." + id + ".enabled") == "on");
        r["order_before"] = ini().get("mods.order");
        m->row(id)->removeButton()->click();
        SheetOverlay* sheet = m->removeSheet();
        QVERIFY(sheet && sheet->isVisible());
        auto* text = sheet->findChild<QLabel*>("mods_remove_text");
        QVERIFY(text && text->text().startsWith("This deletes the folder"));
        r["sheet_text"] = text->text();
        visibleTextIsClean();
        sheet->findChild<FlatButton*>("mods_remove_cancel")->click();
        r["cancel_kept"] = QFileInfo(dir).isDir();
        QVERIFY(QFileInfo(dir).isDir());
        QVERIFY(m->row(id));
        m->row(id)->removeButton()->click();
        sheet = m->removeSheet();
        QVERIFY(sheet);
        sheet->findChild<FlatButton*>("mods_remove_confirm")->click();
        r["folder_gone"] = !QFileInfo::exists(dir);
        r["key_gone"] = !ini().has("mods." + id + ".enabled");
        r["order_after"] = ini().get("mods.order");
        r["notice"] = m->noticeText();
        QVERIFY(!QFileInfo::exists(dir));
        QVERIFY(!ini().has("mods." + id + ".enabled"));
        QVERIFY(!ModList::parseOrder(ini().get("mods.order")).contains(id));
        QVERIFY(!m->row(id));
        QVERIFY(m->noticeText().endsWith("was removed."));
        report_["remove"] = r;
    }

    void copiesAModFolder() {
        ModsPage* m = page();
        const QString src = home_.path() + "/Downloads/Copied Mod";
        QVERIFY(writeFile(src + "/mod.json", R"J({"schema": 2, "id": "copied", "kind": "disc", "name": "Copied mod", "version": "1", "base": {"region": "PAL"},
            "files": [{"path": "SYSTEM.CNF", "form": "replace"}]})J"));
        QVERIFY(writeFile(src + "/disc/SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\n"));
        m->dropPath(src);
        QJsonObject c{{"notice", m->noticeText()}, {"error", m->noticeIsError()}, {"row", m->row("copied") != nullptr},
                      {"copied_file", QFileInfo::exists(mods_ + "/copied/disc/SYSTEM.CNF")}};
        QCOMPARE(m->noticeText(), QStringLiteral("Copied mod was added. Switch it on below; it takes effect the next time the game starts."));
        QVERIFY(m->row("copied"));
        QVERIFY(QFileInfo::exists(mods_ + "/copied/disc/SYSTEM.CNF"));
        QVERIFY(!m->replaceSheet());
        // the same id again: ONE RULE -- asked first; declined, nothing changes
        QString err;
        QVERIFY(app_->setSettingValue("mods.copied.enabled", "on", &err));
        QVERIFY(writeFile(src + "/disc/SYSTEM.CNF", "BOOT2 = cdrom0:\\SLES_525.68;1\r\nVER = 9.99\r\n"));
        m->dropPath(src);
        SheetOverlay* sheet = m->replaceSheet();
        QVERIFY(sheet && sheetButton(sheet, "mods_replace_cancel") && sheetButton(sheet, "mods_replace_confirm"));
        QJsonObject declined{{"title", sheetTitle(sheet)}, {"text", sheetText(sheet)}};
        QCOMPARE(sheetTitle(sheet), QStringLiteral("Replace Copied mod 1?"));
        QCOMPARE(sheetText(sheet), QStringLiteral("The copy you have is removed first."));
        sheetButton(sheet, "mods_replace_cancel")->click();
        QTest::qWait(20);
        QFile kept(mods_ + "/copied/disc/SYSTEM.CNF");
        QVERIFY(kept.open(QIODevice::ReadOnly));
        declined["notice"] = m->noticeText();
        declined["kept_old_copy"] = !kept.readAll().contains("9.99");
        kept.close();   // an open file holds its folder on Windows: the replace below renames it
        declined["sheet_closed"] = m->replaceSheet() == nullptr;
        QCOMPARE(m->noticeText(), QStringLiteral("Nothing was added: you keep Copied mod 1."));
        QVERIFY(declined["kept_old_copy"].toBool() && declined["sheet_closed"].toBool());
        c["declined"] = declined;
        // accepted: the new copy takes the old one's place, its switch and its place in the order kept
        m->dropPath(src);
        sheet = m->replaceSheet();
        QVERIFY(sheet);
        sheetButton(sheet, "mods_replace_confirm")->click();
        QTest::qWait(20);
        QFile now(mods_ + "/copied/disc/SYSTEM.CNF");
        QVERIFY(now.open(QIODevice::ReadOnly));
        QJsonObject accepted{{"notice", m->noticeText()}, {"error", m->noticeIsError()}, {"new_copy", now.readAll().contains("9.99")},
                             {"switch_kept", ini().get("mods.copied.enabled") == QLatin1String("on")}, {"row", m->row("copied") != nullptr},
                             {"left_in_import_tmp", QFileInfo::exists(mods_ + "/.import-tmp")}};
        QCOMPARE(m->noticeText(), QStringLiteral("Copied mod replaced the copy you had; its switch and its place in the order are kept. It takes "
                                                 "effect the next time the game starts."));
        QVERIFY(accepted["new_copy"].toBool() && accepted["switch_kept"].toBool() && m->row("copied") && m->row("copied")->on());
        QVERIFY(!accepted["left_in_import_tmp"].toBool());
        c["accepted"] = accepted;
        QVERIFY(app_->setSettingValue("mods.copied.enabled", "off", &err));
        // a mod made over the North American disc, on this European game folder: its badge, the summary's region, its notes,
        // the Crate Mod Loader settings it asks for, and -- its archive recipe records the North American archive it
        // was made over, which the game's base check refuses on this disc -- "Left out on this disc" first in the warning
        // colour
        QString usa = qEnvironmentVariable("PS2R_MODS_USA");
        if (usa.isEmpty()) {
            usa = home_.path() + "/Downloads/USA Mod";
            QVERIFY(writeFile(usa + "/mod.json", R"J({"schema": 2, "id": "usa_copied", "kind": "disc", "name": "USA mod", "author": "Someone", "version": "v1.64",
                "notes": ["For the mod to work, apply Classic Slide Jump for Crash and Classic Explosion Damage."],
                "base": {"region": "NTSC-U", "executable": "SLUS_209.09"},
                "files": [{"path": "CRASH6/CRASH.BD", "form": "ranges", "bytes": 1041890518, "base_bytes": 890851747},
                          {"path": "CRASH6/MUSIC.MB", "form": "replace"}],
                "import": {"game_code_settings": ["Classic Slide Jump for Crash", "Classic Explosion Damage"]}})J"));
            QVERIFY(writeFile(usa + "/disc/CRASH6/CRASH.BD.ranges", "# a test's recipe (no game data)\nzero 2048\n"));
            QVERIFY(writeFile(usa + "/disc/CRASH6/MUSIC.MB", "music"));
        }
        QString usaId;
        QVERIFY2(readyId(usa, &usaId), qPrintable(usa));
        m->dropPath(usa);
        QVERIFY2(m->row(usaId), qPrintable(m->noticeText()));
        c["usa_row"] = rowJson(m->row(usaId), "folder", int(m->folderOrder().indexOf(usaId)));
        c["disc_region"] = app_->importBaseRegion();
        QCOMPARE(m->row(usaId)->regionText(), QStringLiteral("NTSC-U"));
        QVERIFY2(m->row(usaId)->summaryText().endsWith(QStringLiteral(", made for the North American disc.")), qPrintable(m->row(usaId)->summaryText()));
        QVERIFY2(m->row(usaId)->warningText().startsWith(QStringLiteral("Left out on this disc: made for the North American disc.\n")),
                 qPrintable(m->row(usaId)->warningText()));
        visibleTextIsClean();
        report_["copy"] = c;
    }

    void importsADrop() {
        ModsPage* m = page();
        const QStringList before = m->folderOrder();
        QJsonObject i{{"artefact", crate_}, {"rows_before", QJsonArray::fromStringList(before)}};
        m->dropPath(crate_);
        QVERIFY(QTest::qWaitFor([&] { return !m->importing(); }, 600000));
        QTest::qWait(50);
        i["notice"] = m->noticeText();
        i["error"] = m->noticeIsError();
        const QStringList after = m->folderOrder();
        i["rows_after"] = QJsonArray::fromStringList(after);
        QStringList added;
        for (const QString& id : after) if (!before.contains(id)) added << id;
        i["new_rows"] = QJsonArray::fromStringList(added);
        if (added.size() == 1) {
            ModRow* r = m->row(added.first());
            i["new_row"] = rowJson(r, "folder", int(after.indexOf(added.first())));
            i["new_mod_json"] = QFileInfo::exists(mods_ + "/" + added.first() + "/mod.json");
        }
        visibleTextIsClean();
#ifdef PS2R_LAUNCHER_IMPORT_library
        QVERIFY2(added.size() == 1 && !m->noticeIsError(), qPrintable(m->noticeText()));
#else
        QCOMPARE(m->noticeText(), QStringLiteral("%1 could not be added. Importing is not available in this version of the launcher.").arg(QFileInfo(crate_).fileName()));
        QVERIFY(m->noticeIsError());
        QVERIFY(added.isEmpty());
#endif
        report_["import"] = i;
    }

    void replacesAnImport() {
        // the same crate again: its id is in the folder -- asked first; declined nothing, accepted the import replaces it
        ModsPage* m = page();
        const QStringList before = m->folderOrder();
        const QString id = QString::fromStdString(ModImporter::modIdFor(crate_));
        QJsonObject r{{"id", id}, {"rows_before", QJsonArray::fromStringList(before)}};
#ifdef PS2R_LAUNCHER_IMPORT_library
        QVERIFY2(!id.isEmpty() && before.contains(id), qPrintable(id));
        m->dropPath(crate_);
        SheetOverlay* sheet = m->replaceSheet();
        QVERIFY(sheet && !m->importing());
        r["title"] = sheetTitle(sheet);
        r["text"] = sheetText(sheet);
        sheetButton(sheet, "mods_replace_cancel")->click();
        QTest::qWait(20);
        r["declined_notice"] = m->noticeText();
        r["declined_importing"] = m->importing();
        QVERIFY(!m->importing() && m->noticeText().startsWith("Nothing was added: you keep "));
        const QDateTime stamp = QFileInfo(mods_ + "/" + id + "/mod.json").lastModified();
        QTest::qWait(1100);   // a replaced mod.json is a new file: its time moves on
        m->dropPath(crate_);
        sheet = m->replaceSheet();
        QVERIFY(sheet);
        sheetButton(sheet, "mods_replace_confirm")->click();
        QVERIFY(QTest::qWaitFor([&] { return !m->importing(); }, 600000));
        QTest::qWait(50);
        r["accepted_notice"] = m->noticeText();
        r["accepted_error"] = m->noticeIsError();
        r["rows_after"] = QJsonArray::fromStringList(m->folderOrder());
        r["mod_json_rewritten"] = QFileInfo(mods_ + "/" + id + "/mod.json").lastModified() > stamp;
        QVERIFY2(!m->noticeIsError() && m->noticeText().contains("replaced the copy you had"), qPrintable(m->noticeText()));
        QCOMPARE(m->folderOrder(), before);
        QVERIFY(r["mod_json_rewritten"].toBool());
#else
        QVERIFY(id.isEmpty());   // the stub knows no ids: nothing is asked, the import is refused as before
#endif
        visibleTextIsClean();
        report_["replace_import"] = r;
    }

    void cancelsAnImport() {
        ModsPage* m = page();
        const QStringList before = m->folderOrder();
        m->dropPath(crate_);
        if (SheetOverlay* sheet = m->replaceSheet())   // its id is in the folder (the import above): replace, then stop at once
            sheetButton(sheet, "mods_replace_confirm")->click();
        QVERIFY(m->importing() && m->cancelButton()->isEnabled());
        m->cancelButton()->click();   // before the worker's first step: the library stops at it
        QVERIFY(QTest::qWaitFor([&] { return !m->importing(); }, 600000));
        QTest::qWait(50);
        const QDir tmp(mods_ + "/.import-tmp");
        const QStringList left = tmp.exists() ? tmp.entryList(QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden) : QStringList();
        QJsonObject c{{"notice", m->noticeText()}, {"error", m->noticeIsError()}, {"rows_after", QJsonArray::fromStringList(m->folderOrder())},
                      {"left_in_import_tmp", QJsonArray::fromStringList(left)}};
        QCOMPARE(m->noticeText(), QStringLiteral("Adding %1 was stopped; nothing was added.").arg(QFileInfo(crate_).fileName()));
        QVERIFY(!m->noticeIsError());
        QCOMPARE(m->folderOrder(), before);
        QVERIFY2(left.isEmpty(), qPrintable(left.join(", ")));
        report_["cancel"] = c;
    }

    void importNeedsADisc() {
        ModsPage* m = page();
        QFile::remove(GameFolder(app_->manifest(), folder_).discFile());   // no disc-image.txt, no disc on the Games page
        QVERIFY(app_->importBaseDisc().isEmpty());
        m->dropPath(crate_);
        QJsonObject n{{"notice", m->noticeText()}, {"error", m->noticeIsError()}, {"importing", m->importing()}};
        QCOMPARE(m->noticeText(), QStringLiteral("Add your disc image on the Games page first: a mod is made against your disc."));
        QVERIFY(!m->importing());
        report_["no_disc"] = n;
        QString err;
        QVERIFY(app_->setDiscSetting(iso_, &err));
        report_["visible_text_clean"] = true;
    }
};

QTEST_MAIN(TestLauncherMods)
#include "test_launcher_mods.moc"
