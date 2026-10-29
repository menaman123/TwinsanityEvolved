// tests/app/launcher/test_launcher_settings.cpp -- the Settings page against the game folder's ps2r.ini, row by row (the
// settings judge's driver): for every row of the manifest, every value set through the row's own control (written by
// the launcher's model) lands in the file under the row's key, and a value written to the file by someone else (the
// game's Options menu) is what the row shows after a reload. The folder rows write paths relative to the game folder
// when inside it and absolute otherwise and read back absolute; the disc row writes disc-image.txt (PS2R_SETTINGS_ISO,
// a disc image to name). Writes a JSON report of every row and value (PS2R_SETTINGS_REPORT).
//
// The two language rows offer what the game offers for the disc it starts with: with PS2R_SETTINGS_ISO that disc is the
// game folder's (disc-image.txt) before the page is built, and PS2R_SETTINGS_PACKS (a folder of language packs: the repo's
// mods/) is copied into the game folder's mods folder as the install places the release's; their values are the row's own
// choices. The legacy game.language (its retired "<text>+<speech>" form) is read as the two rows, and a change through a row
// writes both keys and drops it.
//
// Environment: PS2R_SETTINGS_MANIFEST (title.json), HOME (the game folder is made under it), PS2R_SETTINGS_REPORT,
// PS2R_SETTINGS_ISO, PS2R_SETTINGS_PACKS (optional).
#include <QtTest/QtTest>
#include <QDirIterator>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include "app_data.h"
#include "disc_scanner.h"
#include "flat_button.h"
#include "folder_row.h"
#include "game_folder.h"
#include "ini_settings.h"
#include "launcher_app.h"
#include "launcher_style.h"
#include "main_window.h"
#include "segmented_control.h"
#include "settings_page.h"
#include "settings_row.h"
#include "switch_control.h"
#include "title_manifest.h"
using namespace ps2r::launcher;

class TestLauncherSettings : public QObject {
    Q_OBJECT
    LauncherApp* app_ = nullptr;
    MainWindow* window_ = nullptr;
    QJsonArray rows_;
    QString folder_;

    void setThroughControl(SettingsRow* row, const QString& value) {
        if (row->segmented()) {
            const int i = row->segmented()->values().indexOf(value);
            QVERIFY(i >= 0);
            // a click on the segment already selected changes nothing (as for a user): go through a neighbour first
            if (row->segmented()->currentIndex() == i) row->segmented()->setCurrentIndex(i == 0 ? 1 : i - 1, true);
            row->segmented()->setCurrentIndex(i, true);   // the click on that segment
        } else if (row->toggle()) {
            const bool on = value == "on";
            if (row->toggle()->isChecked() == on) row->toggle()->click();
            row->toggle()->click();
        } else {
            // the popup: the menu's action for that value, taken directly (QMenu::exec blocks on a real menu)
            row->setValue(value);
            emit row->valueChanged(row->spec().key, value);
        }
    }

private slots:
    void initTestCase() {
        LauncherStyle::apply();
        app_ = new LauncherApp(this);
        QString err;
        QVERIFY2(app_->loadManifest(qEnvironmentVariable("PS2R_SETTINGS_MANIFEST"), &err), qPrintable(err));
        // the game folder as the setup makes it (its layout and ps2r.ini), without a download
        folder_ = GameFolder::suggested(app_->manifest());
        app_->setChosenFolder(folder_);
        QVERIFY2(GameFolder(app_->manifest(), folder_).prepare(app_->settingDefaults(), &err), qPrintable(err));
        const QString iso = qEnvironmentVariable("PS2R_SETTINGS_ISO");
        if (!iso.isEmpty()) QVERIFY2(GameFolder(app_->manifest(), folder_).setDisc(QFileInfo(iso).absoluteFilePath(), &err), qPrintable(err));
        const QString packs = qEnvironmentVariable("PS2R_SETTINGS_PACKS");
        if (!packs.isEmpty()) {
            for (const QFileInfo& d : QDir(packs).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
                QFile m(d.absoluteFilePath() + "/mod.json");
                if (!m.open(QIODevice::ReadOnly) || QJsonDocument::fromJson(m.readAll()).object().value("kind").toString() != "language") continue;
                const QString dest = app_->modsFolder() + "/" + d.fileName();
                QDirIterator it(d.absoluteFilePath(), QDir::Files, QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    const QString from = it.next();
                    const QString to = dest + from.mid(d.absoluteFilePath().size());
                    QDir().mkpath(QFileInfo(to).absolutePath());
                    QVERIFY(QFile::copy(from, to));
                }
            }
        }
        window_ = new MainWindow(app_);
        window_->show();
        QVERIFY(QTest::qWaitForWindowExposed(window_));
        window_->showPage(MainWindow::SettingsIndex);
    }
    void cleanupTestCase() {
        const QString path = qEnvironmentVariable("PS2R_SETTINGS_REPORT");
        if (!path.isEmpty()) {
            QJsonObject report;
            report["settings_file"] = app_->settingsFilePath();
            report["game_folder"] = folder_;
            report["rows"] = rows_;
            QFile ini(app_->settingsFilePath());
            if (ini.open(QIODevice::ReadOnly)) report["settings_text"] = QString::fromUtf8(ini.readAll());
            QFile f(path);
            if (f.open(QIODevice::WriteOnly)) f.write(QJsonDocument(report).toJson(QJsonDocument::Indented));
        }
        delete window_;
    }
    void everyRowRoundTrips() {
        SettingsPage* page = window_->settingsPage();
        IniSettings ini(app_->settingsFilePath());
        QVERIFY(ini.exists());
        const QVector<SettingsRowSpec> specs = app_->manifest()->allSettingsRows();
        QVERIFY(!specs.isEmpty());
        for (const SettingsRowSpec& spec : specs) {
            QJsonObject r;
            r["key"] = spec.key;
            r["label"] = spec.label;
            r["kind"] = spec.kind;
            if (spec.kind == QLatin1String("folder")) {
                FolderRow* fr = page->folderRow(spec.key);
                QVERIFY2(fr, qPrintable(spec.key));
                r["shown_label"] = fr->labelText();
                QJsonArray values, fileValues, readBack;
                // inside the game folder: written relative; outside: absolute; read back absolute both times
                const QString inside = folder_ + "/" + spec.defaultValue + "-moved";
                const QString outside = QDir::cleanPath(AppData::homePath() + "/Elsewhere/" + spec.defaultValue);
                for (const QString& dir : {inside, outside, folder_ + "/" + spec.defaultValue}) {
                    QVERIFY(page->chooseFolder(spec.key, dir));
                    values << dir;
                    fileValues << ini.get(spec.key);
                    readBack << app_->folderSetting(spec.key);
                    QCOMPARE(app_->folderSetting(spec.key), dir);
                    QCOMPARE(fr->path(), dir);
                }
                QCOMPARE(fileValues[0].toString(), spec.defaultValue + "-moved");
                QCOMPARE(fileValues[1].toString(), outside);
                // the game's menu (or a person) writes the file: the page reads it back
                QVERIFY(ini.set(spec.key, spec.defaultValue));
                page->reload();
                QCOMPARE(fr->path(), folder_ + "/" + spec.defaultValue);
                r["values"] = values;
                r["file_values"] = fileValues;
                r["read_back"] = readBack;
                r["round_trip"] = true;
                rows_ << r;
                continue;
            }
            if (spec.kind == QLatin1String("disc")) {
                const QString iso = qEnvironmentVariable("PS2R_SETTINGS_ISO");
                r["file"] = GameFolder(app_->manifest(), folder_).discFile();
                if (iso.isEmpty()) {
                    r["round_trip"] = QJsonValue();
                    r["note"] = "no disc image given (PS2R_SETTINGS_ISO): the disc row was not exercised";
                    rows_ << r;
                    continue;
                }
                QSignalSpy found(app_->scanner(), &DiscScanner::found);
                page->chooseDisc(iso);
                QVERIFY(found.wait(60000));
                QVERIFY(QTest::qWaitFor([&] { return app_->discSetting() == QFileInfo(iso).absoluteFilePath(); }, 5000));
                r["values"] = QJsonArray{QFileInfo(iso).absoluteFilePath()};
                r["read_back"] = QJsonArray{app_->discSetting()};
                r["round_trip"] = true;
                rows_ << r;
                continue;
            }
            SettingsRow* row = page->row(spec.key);
            QVERIFY2(row, qPrintable(spec.key));
            QJsonArray values, fileValues;
            // a derived row (the languages) offers the disc's choices: the row's own, not the manifest's
            const QVector<SettingsChoice> choices = spec.choicesFrom.isEmpty() ? spec.choices : row->choices();
            if (!spec.choicesFrom.isEmpty()) r["choices_from"] = spec.choicesFrom;
            for (const SettingsChoice& c : choices) {
                setThroughControl(row, c.value);   // the launcher's model writes the key
                QCOMPARE(row->value(), c.value);
                const QString inFile = ini.get(spec.key);   // read the file
                QVERIFY2(inFile == c.value, qPrintable(spec.key + " expected " + c.value + " in the file, got " + inFile));
                page->reload();                             // read it back through the model
                QCOMPARE(row->value(), c.value);
                values << c.value;
                fileValues << inFile;
            }
            // file -> launcher: the game's menu wrote the first value; the page reads it back
            const QString back = choices.first().value;
            QVERIFY(ini.set(spec.key, back));
            page->reload();
            QCOMPARE(row->value(), back);
            r["values"] = values;
            r["file_values"] = fileValues;
            r["round_trip"] = true;
            rows_ << r;
        }
        // the two language rows and the legacy key: game.language (its retired two-name form) read as both rows; a change
        // through one row writes both keys and drops the legacy line
        SettingsRow* text = page->row("game.text_language");
        SettingsRow* speech = page->row("game.speech_language");
        if (text && speech) {
            QJsonObject legacy;
            QVERIFY(ini.remove("game.text_language") && ini.remove("game.speech_language"));
            QVERIFY(ini.set("game.language", "arabic+french"));
            page->reload();
            legacy["read_text"] = text->value();
            legacy["read_speech"] = speech->value();
            const bool both = text->value() == "arabic" && speech->value() == "french";
            legacy["read_as_two"] = both;
            if (both) {
                setThroughControl(speech, "german");
                legacy["written_text"] = ini.get("game.text_language");
                legacy["written_speech"] = ini.get("game.speech_language");
                legacy["legacy_line_left"] = ini.has("game.language");
                QCOMPARE(ini.get("game.text_language"), QStringLiteral("arabic"));
                QCOMPARE(ini.get("game.speech_language"), QStringLiteral("german"));
                QVERIFY(!ini.has("game.language"));
            }
            QJsonObject rr;
            rr["key"] = "game.language";
            rr["legacy"] = legacy;
            rows_ << rr;
        }
        // the frame-rate rule when the manifest has both rows
        SettingsRow* rate = page->row("video.frame_rate");
        SettingsRow* vsync = page->row("video.vsync");
        if (rate && vsync) {
            setThroughControl(rate, "uncapped");
            QVERIFY(!vsync->toggle()->isEnabled());
            setThroughControl(rate, "120");
            QVERIFY(vsync->toggle()->isEnabled());
            QCOMPARE(ini.get("video.frame_rate"), QStringLiteral("120"));
        }
    }
};

QTEST_MAIN(TestLauncherSettings)
#include "test_launcher_settings.moc"
