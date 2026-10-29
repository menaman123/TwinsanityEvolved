// app/launcher/game_folder.h -- the ONE game folder: the game, ps2r.ini, memcards/,
// mods/ and disc-image.txt side by side, so moving the folder moves everything. Makes the layout (ps2r.ini with the
// game's defaults and the folder keys, relative to the folder; an existing file only gains missing keys), reads and
// writes the folder keys (paths.memcards / paths.mods: relative when inside the folder) and the disc the game starts
// with (disc-image.txt, the file the game reads). Plain files; the only writer of ps2r.ini is IniSettings.
#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
namespace ps2r::launcher {
class TitleManifest;

class GameFolder {
public:
    GameFolder(const TitleManifest* manifest, const QString& folder);
    QString path() const { return folder_; }
    QString settingsFile() const;   // <folder>/ps2r.ini
    QString discFile() const;       // <folder>/disc-image.txt ("" when the title names none)
    QString file(const QString& name) const;

    // The folder, ps2r.ini (the defaults for every key it lacks), memcards/ and mods/ (where ps2r.ini points).
    bool prepare(const QVector<QPair<QString, QString>>& defaults, QString* error) const;
    // A folder key's directory, absolute: ps2r.ini's value (relative to the folder) or `fallbackName` inside it.
    QString folderSetting(const QString& key, const QString& fallbackName) const;
    // Writes a folder key: relative when `dir` is inside the game folder, else absolute.
    bool setFolderSetting(const QString& key, const QString& dir, QString* error) const;
    QString toIniPath(const QString& absolute) const;
    QString fromIniPath(const QString& value) const;

    QString disc() const;   // disc-image.txt's path, or ""
    bool setDisc(const QString& iso, QString* error) const;

    // ~/Games/<title> (Windows: %USERPROFILE%\Games\<title>)
    static QString suggested(const TitleManifest* manifest);
    // A picked folder: the folder itself when it is named for the title or already holds the game, else <picked>/<title>.
    static QString resolveChoice(const QString& picked, const TitleManifest* manifest);
    // The folder already holds the game (its settings file, executable or PS Vita package).
    static bool holdsGame(const QString& dir, const TitleManifest* manifest);
    // The nearest existing folder up from `dir` accepts a new file (probe written and removed).
    static bool writable(const QString& dir);
    static qint64 freeBytes(const QString& dir);   // on the drive that would hold `dir`, -1 when unknown

private:
    const TitleManifest* manifest_;
    QString folder_;
};

}  // namespace ps2r::launcher
