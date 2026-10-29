// app/launcher/title_manifest.h -- the per-title manifest (`title.json`) a title ships beside the launcher: who the
// game is, how its discs are recognised (the disc tables beside the manifest), where its finished releases are, which
// machines they run on, how it is played, its game folder and the settings rows that mirror its options file.
// Everything the launcher knows about a game comes from here; nothing of a title is compiled in.
#pragma once
#include <QHash>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <QVector>
namespace ps2r::launcher {

struct SettingsChoice {
    QString value;    // what is written to the options file
    QString label;    // what the user reads
};

struct SettingsRowSpec {
    QString key;      // the options-file key (video.frame_rate); for kind "disc" the file the game reads
    QString label;
    QString kind;     // "choice", "switch", "folder" (a path, relative to the game folder when inside it), "disc"
    QString defaultValue;
    QString help;
    bool writeDefault = false;   // written into a new options file with its default
    QVector<SettingsChoice> choices;
    // "text_languages" | "speech_languages": the choices are DERIVED -- the languages of the disc the game starts with
    // (and, for the text, the language packs in the game folder's mods folder), not the manifest's list
    QString choicesFrom;
};

struct SettingsGroupSpec {
    QString title;
    QString target;   // "vita": the group of the PS Vita's own settings (the Settings page adds the PS Vita folder's button)
    QVector<SettingsRowSpec> rows;
};

struct ComputerTarget {
    QString id;      // macos-arm64 / macos-x86_64 / windows-x64 / linux-x86_64 / linux-aarch64 (the release zip's name)
    QString label;   // "macOS (Apple silicon)"
    QString shortLabel;   // "macOS"
    QString exe;     // the game's executable in the zip
};

struct BuiltinMod {
    QString id, name, author, help;
    bool hasSwitch = true;   // false: the game has no switch for it (always available; "switch": false in title.json)
};

struct LicenceLink { QString label, path; };

class TitleManifest {
public:
    bool load(const QString& path, QString* error);
    bool loadJson(const QJsonObject& root, QString* error, const QString& baseDir = QString());
    bool valid() const { return valid_; }

    QString path() const { return path_; }
    QString id() const { return id_; }
    QString name() const { return name_; }
    QString product() const { return product_; }
    QString description() const { return description_; }
    QString appDataFolder() const { return appDataFolder_; }
    QString binaryName() const { return binaryName_; }

    // the disc tables -- known dumps, file lists (absolute paths, resolved against the manifest's folder), the programs this release plays
    QStringList discManifests() const { return discManifests_; }
    QStringList playableExecutables() const { return plays_; }
    QString playsNote() const { return playsNote_; }
    QStringList discExtensions() const { return discExtensions_; }

    QString releasesUrl() const { return releasesUrl_; }
    QString pageUrl() const { return pageUrl_; }
    QString zipAssetPattern() const { return zipAsset_; }    // twinsanityevolved-{tag}-{target}.zip
    QString vitaAssetPattern() const { return vitaAsset_; }  // twinsanityevolved-{tag}-vita.vpk
    QString sumsAsset() const { return sumsAsset_; }
    // The launcher's own file for a computer in a release (release.launcher_assets: target -> name pattern with {tag};
    // macOS the .app zipped, Windows the zip, Linux the AppImage); "" when none is published for it
    QString launcherAssetPattern(const QString& target) const { return launcherAssets_.value(target); }
    qint64 installBytes() const { return installBytes_; }

    const QVector<ComputerTarget>& computers() const { return computers_; }
    const ComputerTarget* computer(const QString& id) const;
    QString vitaLabel() const { return vitaLabel_; }
    QString vitaFile() const { return vitaFile_; }
    QStringList vitaNote() const { return vitaNote_; }   // the steps; "{disc}" stands for the disc image's name on the console
    // The PS Vita folder (targets.vita.folder): <game folder>/<name>/ with the package and <data_dir>/ (the console's
    // data folder as a path on its memory card: ux0/data/twinsanity) holding the game folder's settings, its mods and the
    // disc file; the README with the steps; the disc's name on the console when the player's own is not a plain name; the
    // settings keys a copy leaves out (the computer's folders and its game rate: the console has its own)
    QString vitaFolderName() const { return vitaFolderName_; }
    QString vitaDataDir() const { return vitaDataDir_; }
    QString vitaReadme() const { return vitaReadme_; }
    QString vitaDefaultDisc() const { return vitaDefaultDisc_; }
    QStringList vitaSettingsLeftOut() const { return vitaDroppedKeys_; }
    // The PS Vita explained: the discs on the console (targets.vita.discs_note: lines with {disc} {data} {disc_file}),
    // the Mods page's panel (targets.vita.mods_note) and About's section (targets.vita.about: the whole flow, one sentence a line)
    QStringList vitaDiscsNote() const { return vitaDiscsNote_; }
    QString vitaModsNote() const { return vitaModsNote_; }
    QStringList vitaAbout() const { return vitaAbout_; }
    // A target's label: a computer's, or the Vita's for "vita".
    QString targetLabel(const QString& target) const;

    QString runIsoFlag() const { return runIsoFlag_; }
    QString runConfigFlag() const { return runConfigFlag_; }
    QStringList runArgs() const { return runArgs_; }
    QString titleStateLine() const { return titleStateLine_; }
    QString titleStateName() const { return titleStateName_; }

    QString gameFolderDefault() const { return gameFolderDefault_; }
    QString settingsFileName() const { return settingsFileName_; }
    QString memcardsName() const { return memcardsName_; }
    QString modsName() const { return modsName_; }
    QString discFileName() const { return discFileName_; }
    QStringList protectedNames() const { return protectedNames_; }
    QString settingsHeader() const { return settingsHeader_; }

    QString settingsNote() const { return settingsNote_; }
    const QVector<SettingsGroupSpec>& settingsGroups() const { return settingsGroups_; }
    QVector<SettingsRowSpec> allSettingsRows() const;
    const QVector<BuiltinMod>& builtinMods() const { return builtinMods_; }

    QStringList scanFolders() const { return scanFolders_; }
    bool scanRemovable() const { return scanRemovable_; }
    int scanMaxDepth() const { return scanMaxDepth_; }
    qint64 scanMinBytes() const { return scanMinBytes_; }
    const QVector<LicenceLink>& licences() const { return licences_; }

private:
    bool valid_ = false;
    QString path_, id_, name_, product_, description_, appDataFolder_, binaryName_;
    QStringList discManifests_, plays_, discExtensions_;
    QString playsNote_;
    QString releasesUrl_, pageUrl_, zipAsset_, vitaAsset_, sumsAsset_;
    qint64 installBytes_ = 0;
    QVector<ComputerTarget> computers_;
    QString vitaLabel_, vitaFile_;
    QStringList vitaNote_;
    QString vitaFolderName_, vitaDataDir_, vitaReadme_, vitaDefaultDisc_;
    QStringList vitaDroppedKeys_;
    QStringList vitaDiscsNote_, vitaAbout_;
    QString vitaModsNote_;
    QHash<QString, QString> launcherAssets_;
    QString runIsoFlag_, runConfigFlag_, titleStateLine_, titleStateName_;
    QStringList runArgs_;
    QString gameFolderDefault_, settingsFileName_, memcardsName_, modsName_, discFileName_, settingsHeader_;
    QStringList protectedNames_;
    QString settingsNote_;
    QVector<SettingsGroupSpec> settingsGroups_;
    QVector<BuiltinMod> builtinMods_;
    QStringList scanFolders_;
    bool scanRemovable_ = true;
    int scanMaxDepth_ = 2;
    qint64 scanMinBytes_ = 32ll * 1024 * 1024;
    QVector<LicenceLink> licences_;
};

}  // namespace ps2r::launcher
