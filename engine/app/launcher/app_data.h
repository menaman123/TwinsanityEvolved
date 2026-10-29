// app/launcher/app_data.h -- the launcher's own folder in the user's app-data (macOS ~/Library/Application
// Support/<product>, Windows %LOCALAPPDATA%\<product>, Linux ~/.local/share/<product>): its state file (launcher.json:
// the release server, the installs, the discs, the window), its logs and its downloads in progress. The game itself
// lives in the game folder the user chose, never here. One instance per running launcher.
#pragma once
#include <QJsonObject>
#include <QString>
namespace ps2r::launcher {

class AppData {
public:
    explicit AppData(const QString& productFolder);

    QString root() const { return root_; }
    QString logsDir() const;        // Logs/
    QString downloadsDir() const;   // Downloads/ (a download in progress; removed once the game is in its folder)
    QString logPath(const QString& step) const;   // Logs/<step>-<timestamp>.log

    // Expands ~, ${app_data} and ${launcher} (the folder holding the launcher) in a manifest path.
    QString expand(const QString& path) const;
    static QString homePath();
    static QString launcherDir();
    static QString shortPath(const QString& path);   // ~/… for display

    bool ensureFolders(QString* error) const;

    // launcher.json
    QJsonObject state() const { return state_; }
    QJsonValue value(const QString& key, const QJsonValue& fallback = QJsonValue()) const;
    void setValue(const QString& key, const QJsonValue& value);
    void remove(const QString& key);
    bool save(QString* error = nullptr) const;
    QString statePath() const;

private:
    void loadState();
    QString root_;
    QJsonObject state_;
};

}  // namespace ps2r::launcher
