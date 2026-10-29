// app/launcher/game_folder.cpp -- the game folder's layout.
#include "game_folder.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStorageInfo>
#include "app_data.h"
#include "ini_settings.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
QString existingAncestor(const QString& dir) {
    QString d = QDir::cleanPath(dir);
    while (!d.isEmpty() && !QFileInfo(d).isDir()) {
        const QString up = QFileInfo(d).absolutePath();
        if (up == d) return QString();
        d = up;
    }
    return d;
}
}  // namespace

GameFolder::GameFolder(const TitleManifest* manifest, const QString& folder) : manifest_(manifest), folder_(QDir::cleanPath(folder)) {}

QString GameFolder::file(const QString& name) const { return folder_ + "/" + name; }
QString GameFolder::settingsFile() const { return file(manifest_->settingsFileName()); }
QString GameFolder::discFile() const { return manifest_->discFileName().isEmpty() ? QString() : file(manifest_->discFileName()); }

bool GameFolder::prepare(const QVector<QPair<QString, QString>>& defaults, QString* error) const {
    if (!QDir().mkpath(folder_)) {
        if (error) *error = QStringLiteral("%1 could not be made").arg(QDir::toNativeSeparators(folder_));
        return false;
    }
    const QString ini = settingsFile();
    if (!QFileInfo::exists(ini) && !manifest_->settingsHeader().isEmpty()) {
        QSaveFile f(ini);
        if (!f.open(QIODevice::WriteOnly) || f.write((manifest_->settingsHeader() + "\n").toUtf8()) < 0 || !f.commit()) {
            if (error) *error = QStringLiteral("%1 could not be written").arg(QDir::toNativeSeparators(ini));
            return false;
        }
    }
    IniSettings settings(ini);
    for (const QPair<QString, QString>& kv : defaults) {
        if (settings.has(kv.first)) continue;   // the user's (or the game's) value stays
        if (!settings.set(kv.first, kv.second, error)) return false;
    }
    for (const QString& dir : {folderSetting("paths.memcards", manifest_->memcardsName()), folderSetting("paths.mods", manifest_->modsName())}) {
        if (!QDir().mkpath(dir)) {
            if (error) *error = QStringLiteral("%1 could not be made").arg(QDir::toNativeSeparators(dir));
            return false;
        }
    }
    return true;
}

QString GameFolder::toIniPath(const QString& absolute) const {
    const QString clean = QDir::cleanPath(absolute);
    if (clean == folder_) return QStringLiteral(".");
    if (clean.startsWith(folder_ + "/")) return clean.mid(folder_.size() + 1);
    return clean;
}

QString GameFolder::fromIniPath(const QString& value) const {
    if (value.isEmpty()) return QString();
    QString v = value;
    v.replace('\\', '/');
    if (QDir::isAbsolutePath(v) || (v.size() > 1 && v[1] == ':')) return QDir::cleanPath(v);
    return QDir::cleanPath(folder_ + "/" + v);
}

QString GameFolder::folderSetting(const QString& key, const QString& fallbackName) const {
    const QString v = IniSettings(settingsFile()).get(key);
    return v.isEmpty() ? QDir::cleanPath(folder_ + "/" + fallbackName) : fromIniPath(v);
}

bool GameFolder::setFolderSetting(const QString& key, const QString& dir, QString* error) const {
    if (!QDir().mkpath(dir)) {
        if (error) *error = QStringLiteral("%1 could not be made").arg(QDir::toNativeSeparators(dir));
        return false;
    }
    return IniSettings(settingsFile()).set(key, toIniPath(dir), error);
}

QString GameFolder::disc() const {
    const QString f = discFile();
    if (f.isEmpty()) return QString();
    QFile in(f);
    if (!in.open(QIODevice::ReadOnly)) return QString();
    QByteArray line = in.readLine();
    if (line.startsWith("\xEF\xBB\xBF")) line.remove(0, 3);
#ifdef Q_OS_WIN
    return QDir::cleanPath(QString::fromLocal8Bit(line).trimmed().remove('"'));
#else
    return QDir::cleanPath(QString::fromUtf8(line).trimmed().remove('"'));
#endif
}

bool GameFolder::setDisc(const QString& iso, QString* error) const {
    const QString f = discFile();
    if (f.isEmpty()) return true;
    QSaveFile out(f);
    if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        if (error) *error = QStringLiteral("%1 could not be written").arg(QDir::toNativeSeparators(f));
        return false;
    }
#ifdef Q_OS_WIN
    out.write(QDir::toNativeSeparators(iso).toLocal8Bit());   // the game opens it through the system code page
#else
    out.write(iso.toUtf8());
#endif
    out.write("\n");
    if (!out.commit()) {
        if (error) *error = QStringLiteral("%1 could not be written").arg(QDir::toNativeSeparators(f));
        return false;
    }
    return true;
}

QString GameFolder::suggested(const TitleManifest* manifest) {
    QString d = manifest->gameFolderDefault();
    if (d == "~") d = AppData::homePath();
    else if (d.startsWith("~/")) d = AppData::homePath() + d.mid(1);
    return QDir::cleanPath(d);
}

QString GameFolder::resolveChoice(const QString& picked, const TitleManifest* manifest) {
    const QString dir = QDir::cleanPath(picked);
    const QString name = QFileInfo(dir).fileName();
    if (name.startsWith(manifest->name(), Qt::CaseInsensitive)) return dir;   // "Crash Twinsanity", "Crash Twinsanity (PS Vita)"
    if (holdsGame(dir, manifest)) return dir;
    return dir + "/" + manifest->name();
}

bool GameFolder::holdsGame(const QString& dir, const TitleManifest* manifest) {
    QStringList names{manifest->settingsFileName(), manifest->binaryName(), manifest->binaryName() + ".exe"};
    if (!manifest->vitaFile().isEmpty()) names << manifest->vitaFile();
    for (const QString& n : names)
        if (!n.isEmpty() && QFileInfo::exists(dir + "/" + n)) return true;
    return false;
}

bool GameFolder::writable(const QString& dir) {
    const QString base = existingAncestor(dir);
    if (base.isEmpty()) return false;
    QFile probe(base + "/.twinsanityevolved-launcher-probe");
    if (!probe.open(QIODevice::WriteOnly)) return false;
    probe.close();
    probe.remove();
    return true;
}

qint64 GameFolder::freeBytes(const QString& dir) {
    const QString base = existingAncestor(dir);
    if (base.isEmpty()) return -1;
    const QStorageInfo info(base);
    return info.isValid() ? info.bytesAvailable() : -1;
}

}  // namespace ps2r::launcher
