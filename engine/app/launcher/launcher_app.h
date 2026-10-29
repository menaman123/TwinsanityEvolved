// app/launcher/launcher_app.h -- the launcher's model: the manifest, the app-data folder, the discs (identified by
// content), the installs (one game folder per target, the finished game downloaded into it), the running game and the
// game folder's settings file; the window shows it and the test drivers drive it through the same methods the buttons
// call. Nothing here compiles anything (D-A).
#pragma once
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QStringList>
#include <QVector>
#include "disc_scanner.h"
#include "vita_folder.h"
namespace ps2r::launcher {
class AppData;
class GameLibrary;
class GameProcess;
class ReleaseInstaller;
class SelfUpdate;
class TitleManifest;

struct InstallRecord {
    QString target;    // a computer's id (macos-arm64 ...) or "vita"
    QString folder;    // the game folder
    QString tag;       // the release installed (v0.3.0)
    QString placed;    // the executable or the .vpk
    QDateTime at;
};

class LauncherApp : public QObject {
    Q_OBJECT
public:
    explicit LauncherApp(QObject* parent = nullptr);
    ~LauncherApp() override;

    // Finds the manifest: an explicit path, else PS2R_LAUNCHER_MANIFEST, else titles/*.json beside the application
    // (Contents/Resources/titles on macOS); loads the disc tables it names. False with *error when none is found.
    bool loadManifest(const QString& explicitPath, QString* error);
    static QStringList manifestSearchPaths();

    const TitleManifest* manifest() const { return manifest_; }
    AppData* appData() const { return appData_; }
    GameLibrary* library() const { return library_; }
    ReleaseInstaller* installer() const { return installer_; }
    SelfUpdate* selfUpdate() const { return selfUpdate_; }   // the launcher's own update
    GameProcess* game() const { return game_; }
    DiscScanner* scanner() const { return scanner_; }

    // --- installs ---
    QString hostTarget() const;
    QVector<InstallRecord> installs() const;
    InstallRecord installFor(const QString& target) const;   // an empty record when there is none
    InstallRecord playableInstall() const;                     // this computer's install
    bool setupDone() const;                                    // at least one install exists
    // The game folder the Settings page and Play use: this computer's install, else the first install, else the folder
    // chosen in the setup (not installed yet), else "".
    QString gameFolder() const;
    void setChosenFolder(const QString& folder);
    // Downloads the target's game into `folder` (ReleaseInstaller); when it is placed the folder gets its layout
    // (ps2r.ini, memcards, mods; disc-image.txt for this computer) and the install is recorded.
    void startInstall(const QString& target, const QString& folder);
    QString updateFor(const QString& target) const;            // a newer release's tag, or ""
    void checkForUpdates();
    void checkLauncherUpdate();                                // a newer launcher (at start, and from About)

    // --- the PS Vita folder (vita_folder.h) ---
    // A new PS Vita install's folder: <the game folder>/vita (the setup's suggestion and its first-run choice)
    QString vitaFolderFor(const QString& gameFolder) const;
    QString vitaFolder() const;   // the PS Vita install's folder, "" when there is none
    // The disc's name on the console the folder will name (disc-image.txt): the game folder's disc, else the first disc on
    // the Games page, as a plain file name -- else game.iso (VitaFolder::consoleDiscName)
    QString vitaConsoleDisc() const;
    // Makes the PS Vita install's folder ready to copy again: the game folder's settings, its language packs and the mods
    // switched on, disc-image.txt and the README (after the install, and the Settings page's button). False with
    // report->error when it could not (or there is no PS Vita install).
    bool makeVitaFolder(VitaFolderReport* report);
    VitaFolderReport lastVitaFolder() const { return vitaReport_; }

    // --- discs ---
    void scan();
    void identify(const QString& path);    // a chosen or dropped file: added when accepted, discRefused otherwise
    void addDisc(const DiscFound& disc);
    void setDiscsChosen(const QVector<DiscFound>& discs);   // the setup's step 1: the chosen discs become the cards

    // --- play ---
    enum Action { Play, Playing, Missing, NotPlayable, NotInstalled };
    Action nextAction(const QString& isoPath) const;
    QString notPlayableReason(const QString& isoPath) const;
    bool play(const QString& isoPath, QString* error);
    void stopGame();

    // --- the game folder's settings file (ps2r.ini), read on every call, written one key at a time ---
    QString settingsFilePath() const;
    QString settingValue(const QString& key) const;   // the file's value or the manifest's default
    bool setSettingValue(const QString& key, const QString& value, QString* error = nullptr);
    bool removeSettingValue(const QString& key, QString* error = nullptr);   // drops the key's line (a removed mod's)
    QString folderSetting(const QString& key) const;  // paths.memcards / paths.mods, absolute
    bool setFolderSetting(const QString& key, const QString& dir, QString* error = nullptr);
    QString discSetting() const;                       // disc-image.txt
    bool setDiscSetting(const QString& iso, QString* error = nullptr);
    QString modsFolder() const;
    // The disc a mod is imported against: the one the game starts with (disc-image.txt), else the first disc on the Games
    // page that is still where it was; "" when there is none.
    QString importBaseDisc() const;
    // That disc's region as its card says it ("PAL", "NTSC-U", "NTSC-J"); "" when there is no disc or it is not known
    QString importBaseRegion() const;
    QVector<QPair<QString, QString>> settingDefaults() const;   // the rows written into a new ps2r.ini
    // The rows an install adds to the folder's ps2r.ini (only the missing ones are written): settingDefaults(), without the two
    // language rows when the file still has the older game.language (it keeps naming both until a language row is changed)
    QVector<QPair<QString, QString>> settingDefaultsFor(const QString& settingsFile) const;

    // --- the two language rows: what the game offers for the disc it starts with ---
    struct LanguageChoices {
        QStringList text;     // the text languages by content, then the language packs that fit this disc ("arabic")
        QStringList speech;   // the speech languages (the disc's speech banks)
        QString disc;         // the disc they were read from ("" = none known: english only)
    };
    // Read from importBaseDisc() (cached while the disc and the mods folder are unchanged); english alone without a disc
    LanguageChoices languageChoices() const;
    // game.text_language / game.speech_language as the game resolves them: the file's key, else the legacy game.language
    // through the single setting's pairing, else english; a name the row does not offer shows as the game falls back
    QString languageValue(const QString& key) const;
    // A language pack is not a mod: its stale `mods.<pack>.enabled` and its place in `mods.order` are dropped
    void dropLanguagePackKeys();

    QString version() const;
    QString engineCommit() const;

signals:
    void stateChanged();   // anything a card or a row shows
    void settingsChanged(const QString& key);
    void installsChanged();
    void installFinished(bool ok);
    void updateAvailable(const QString& target, const QString& tag);
    void discRefused(const QString& path, const QString& reason);

private:
    void saveInstalls(const QVector<InstallRecord>& installs);
    void finishInstall();
    TitleManifest* manifest_ = nullptr;
    AppData* appData_ = nullptr;
    GameLibrary* library_ = nullptr;
    ReleaseInstaller* installer_ = nullptr;
    SelfUpdate* selfUpdate_ = nullptr;
    GameProcess* game_ = nullptr;
    DiscScanner* scanner_ = nullptr;
    QString playingIso_;
    QHash<QString, QString> updates_;   // target -> the newer tag
    VitaFolderReport vitaReport_;       // the PS Vita folder's last making
    QStringList setupAdded_;            // discs the setup added (a changed choice takes them back)
    mutable QString languagesKey_;      // what languageChoices() last read (the disc and the mods folder's packs)
    mutable LanguageChoices languages_;
};

}  // namespace ps2r::launcher
