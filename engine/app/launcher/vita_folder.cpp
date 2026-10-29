// app/launcher/vita_folder.cpp -- the PS Vita folder.
#include "vita_folder.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QSaveFile>
#include <QSet>
#include "game_folder.h"
#include "ini_settings.h"
#include "mod_list.h"
#include "release_catalogue.h"
#include "title_manifest.h"
namespace ps2r::launcher {

namespace {
constexpr qint64 kRoomMargin = 16ll << 20;   // the room a copy leaves free beside what it writes

bool hidden(const QString& name) { return name.startsWith('.'); }   // .DS_Store, the import's .import-tmp

// Every file under `dir` (hidden names left out), relative to it, with its size
void listFiles(const QString& dir, const QString& rel, QHash<QString, QFileInfo>* out) {
    for (const QFileInfo& fi : QDir(dir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden, QDir::Name)) {
        if (hidden(fi.fileName())) continue;
        const QString r = rel.isEmpty() ? fi.fileName() : rel + "/" + fi.fileName();
        if (fi.isDir()) listFiles(fi.absoluteFilePath(), r, out);
        else out->insert(r, fi);
    }
}

bool sameFile(const QFileInfo& src, const QString& dst) {
    const QFileInfo d(dst);
    return d.isFile() && d.size() == src.size() && d.lastModified() == src.lastModified();
}

bool writeText(const QString& path, const QByteArray& bytes) {
    if (QFile::exists(path)) {
        QFile f(path);
        if (f.open(QIODevice::ReadOnly) && f.readAll() == bytes) return true;   // unchanged: its time stays
    }
    QSaveFile f(path);
    return f.open(QIODevice::WriteOnly | QIODevice::Truncate) && f.write(bytes) == bytes.size() && f.commit();
}

// `from` copied into `to` file by file (a file already there with the same size and time is kept); files of `to` that
// `from` no longer has are removed
bool syncTree(const QString& from, const QString& to, VitaFolderReport* r, QString* failed) {
    QHash<QString, QFileInfo> src, dst;
    listFiles(from, QString(), &src);
    listFiles(to, QString(), &dst);
    for (auto it = dst.cbegin(); it != dst.cend(); ++it) {
        if (src.contains(it.key())) continue;
        if (!QFile::remove(it.value().absoluteFilePath())) { *failed = it.value().absoluteFilePath(); return false; }
        r->removed++;
    }
    for (auto it = src.cbegin(); it != src.cend(); ++it) {
        const QString dest = to + "/" + it.key();
        if (sameFile(it.value(), dest)) { r->kept++; continue; }
        if (!QDir().mkpath(QFileInfo(dest).absolutePath())) { *failed = QFileInfo(dest).absolutePath(); return false; }
        QFile::remove(dest);
        if (!QFile::copy(it.value().absoluteFilePath(), dest)) { *failed = dest; return false; }
        QFile out(dest);
        out.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther);
        if (out.open(QIODevice::ReadWrite)) {   // the source's time: the next copy keeps it
            out.setFileTime(it.value().lastModified(), QFileDevice::FileModificationTime);
            out.close();
        }
        r->copied++;
    }
    return true;
}

// The bytes a sync of `from` into `to` would write
qint64 bytesToWrite(const QString& from, const QString& to) {
    QHash<QString, QFileInfo> src;
    listFiles(from, QString(), &src);
    qint64 n = 0;
    for (auto it = src.cbegin(); it != src.cend(); ++it)
        if (!sameFile(it.value(), to + "/" + it.key())) n += it.value().size();
    return n;
}

qint64 treeBytes(const QString& dir) {
    QHash<QString, QFileInfo> files;
    listFiles(dir, QString(), &files);
    qint64 n = 0;
    for (const QFileInfo& fi : files) n += fi.size();
    return n;
}
}  // namespace

QString VitaFolder::folderIn(const TitleManifest* m, const QString& gameFolder) {
    return QDir::cleanPath(gameFolder + "/" + (m->vitaFolderName().isEmpty() ? QStringLiteral("vita") : m->vitaFolderName()));
}

QString VitaFolder::homeOf(const TitleManifest* m, const QString& folder) {
    const QFileInfo fi(QDir::cleanPath(folder));
    const QString name = m->vitaFolderName().isEmpty() ? QStringLiteral("vita") : m->vitaFolderName();
    return fi.fileName().compare(name, Qt::CaseInsensitive) == 0 ? fi.absolutePath() : QString();
}

QString VitaFolder::consolePath(const QString& dataDir) {
    QString p = QDir::fromNativeSeparators(dataDir);
    const int slash = p.indexOf('/');
    if (slash > 0 && !p.left(slash).contains(':')) p[slash] = ':';   // ux0/data/twinsanity -> ux0:data/twinsanity
    return p;
}

QString VitaFolder::consoleDiscName(const TitleManifest* m, const QString& disc) {
    const QString fallback = m->vitaDefaultDisc().isEmpty() ? QStringLiteral("game.iso") : m->vitaDefaultDisc();
    const QString name = QFileInfo(QString(disc).replace('\\', '/')).fileName();   // a Windows path read anywhere: its file name
    if (name.isEmpty() || name.size() > 128 || name.startsWith('.') || name.startsWith(' ') || name.endsWith('.') || name.endsWith(' '))
        return fallback;
    for (const QChar c : name) {
        const ushort u = c.unicode();
        if (u < 0x20 || u > 0x7E || QStringLiteral("\\/:*?\"<>|").contains(c)) return fallback;
    }
    bool known = m->discExtensions().isEmpty();
    for (const QString& e : m->discExtensions()) known |= name.endsWith(e, Qt::CaseInsensitive);
    return known ? name : fallback;
}

QString VitaFolder::settingsText(const TitleManifest* m, const QString& settingsFile) {
    QFile f(settingsFile);
    if (settingsFile.isEmpty() || !f.open(QIODevice::ReadOnly)) return QString();
    QString text = QString::fromUtf8(f.readAll());
    for (const QString& key : m->vitaSettingsLeftOut()) text = IniSettings::removeFromText(text, key);
    return text;
}

QString VitaFolder::fill(const TitleManifest* m, QString line, const QString& disc) {
    const QString data = consolePath(m->vitaDataDir());
    const QString discFile = m->discFileName().isEmpty() ? QStringLiteral("disc-image.txt") : m->discFileName();
    line.replace(QLatin1String("{disc}"), disc).replace(QLatin1String("{data}"), data).replace(QLatin1String("{data_dir}"), m->vitaDataDir())
        .replace(QLatin1String("{vpk}"), m->vitaFile()).replace(QLatin1String("{disc_file}"), discFile);
    return line;
}

QStringList VitaFolder::steps(const TitleManifest* m, const QString& disc) {
    QStringList out;
    for (const QString& line : m->vitaNote()) out << fill(m, line, disc);
    return out;
}

QStringList VitaFolder::discsNote(const TitleManifest* m, const QString& disc) {
    QStringList out;
    for (const QString& line : m->vitaDiscsNote()) out << fill(m, line, disc);
    return out;
}

QString VitaFolder::modsNote(const TitleManifest* m, const QString& disc) { return fill(m, m->vitaModsNote(), disc); }

QStringList VitaFolder::about(const TitleManifest* m, const QString& disc) {
    QStringList out;
    for (const QString& line : m->vitaAbout()) out << fill(m, line, disc);
    return out;
}

QString VitaFolder::summary(const VitaFolderReport& r) {
    if (r.mods.isEmpty()) return QStringLiteral("No mods are switched on, so none are copied.");
    return QStringLiteral("The mods take %1 on the memory card (%2).")
        .arg(ReleaseCatalogue::bytesText(r.modBytes), (r.names.size() == r.mods.size() ? r.names : r.mods).join(QStringLiteral(", ")));
}

QString VitaFolder::readme(const TitleManifest* m, const VitaFolderReport& r) {
    const QString data = consolePath(m->vitaDataDir());
    QString t;
    const QString title = QStringLiteral("%1 for the %2").arg(m->name(), m->vitaLabel().isEmpty() ? QStringLiteral("PS Vita") : m->vitaLabel());
    t += title + "\n" + QString(title.size(), '=') + "\n\n";
    t += QStringLiteral("This folder is ready to copy to your %1.\n\n").arg(m->vitaLabel());
    int n = 1;
    for (const QString& s : steps(m, r.disc)) t += QStringLiteral("%1. %2\n").arg(n++).arg(s);
    t += "\n";
    // the discs on the console: the name disc-image.txt gives, and the two regions side by side
    const QStringList discs = discsNote(m, r.disc);
    if (!discs.isEmpty()) t += discs.join(QStringLiteral("\n")) + "\n\n";
    if (!r.dataDir.isEmpty()) {
        t += summary(r) + "\n";
        t += QStringLiteral("Your saves are not copied: the %1 keeps its own memory card in %2/memcards.\n").arg(m->vitaLabel(), data);
        if (discs.isEmpty())   // the manifest's discs note says it at length when there is one
            t += QStringLiteral("Another disc: copy it beside %1 in %2 and write its file name in %2/%3.\n")
                     .arg(r.disc, data, m->discFileName().isEmpty() ? QStringLiteral("disc-image.txt") : m->discFileName());
        t += QStringLiteral("After you add a mod, switch one on or off or change a setting in the launcher, choose \"Make the %1 folder again\" "
                            "(on its Mods or Settings page) and copy %2 again.\n").arg(m->vitaLabel(), m->vitaDataDir());
    }
    return t;
}

bool VitaFolder::make(const TitleManifest* m, const QString& gameFolder, const QString& folder, const QString& fallbackDisc,
                      VitaFolderReport* report) {
    VitaFolderReport r;
    r.folder = QDir::cleanPath(folder);
    auto fail = [&](const QString& why) {
        r.error = why;
        *report = r;
        return false;
    };
    if (!QDir().mkpath(r.folder)) return fail(QStringLiteral("%1 could not be made.").arg(QDir::toNativeSeparators(r.folder)));
    const bool haveGame = !gameFolder.isEmpty();
    const GameFolder gf(m, haveGame ? gameFolder : r.folder);
    // the disc the console starts with: the one the game folder starts with, else the fallback
    QString disc = haveGame ? gf.disc() : QString();
    if (disc.isEmpty() || !QFileInfo::exists(disc)) disc = fallbackDisc;
    r.discFrom = disc;
    r.disc = consoleDiscName(m, disc);
    if (!m->vitaDataDir().isEmpty()) {
        r.dataDir = QDir::cleanPath(r.folder + "/" + m->vitaDataDir());
        // the mods: the language packs and the mods switched on, in the overlay order
        const IniSettings ini(gf.settingsFile());
        const QString modsDir = haveGame ? gf.folderSetting(QStringLiteral("paths.mods"), m->modsName()) : QString();
        QStringList ids, names;
        QHash<QString, QString> dirs;
        if (!modsDir.isEmpty() && QFileInfo(modsDir).isDir()) {
            for (const ModEntry& e : ModList::ordered(ModList::read(modsDir), ini.get(QStringLiteral("mods.order")))) {
                if (!e.readable) continue;
                if (!ModList::isLanguagePack(e) && ini.get(QStringLiteral("mods.%1.enabled").arg(e.id)) != QLatin1String("on")) continue;
                ids << e.id;
                names << e.name;
                dirs.insert(e.id, e.dir);
            }
        }
        const QString copyDir = r.dataDir + "/" + (m->modsName().isEmpty() ? QStringLiteral("mods") : m->modsName());
        qint64 need = 0;
        for (const QString& id : ids) need += bytesToWrite(dirs.value(id), copyDir + "/" + id);
        const qint64 room = GameFolder::freeBytes(r.folder);
        if (need > 0 && room >= 0 && room < need + kRoomMargin)
            return fail(QStringLiteral("There is not enough room in %1 for the mods (%2 to copy, %3 free).")
                            .arg(QDir::toNativeSeparators(r.folder), ReleaseCatalogue::bytesText(need), ReleaseCatalogue::bytesText(room)));
        if (!QDir().mkpath(copyDir)) return fail(QStringLiteral("%1 could not be made.").arg(QDir::toNativeSeparators(copyDir)));
        // a mod no longer copied leaves the copy (only inside the copy's own mods folder)
        for (const QFileInfo& fi : QDir(copyDir).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden)) {
            if (ids.contains(fi.fileName())) continue;
            const bool gone = fi.isDir() && !fi.isSymLink() ? QDir(fi.absoluteFilePath()).removeRecursively() : QFile::remove(fi.absoluteFilePath());
            if (!gone) return fail(QStringLiteral("%1 could not be removed.").arg(QDir::toNativeSeparators(fi.absoluteFilePath())));
            r.removed++;
        }
        for (const QString& id : ids) {
            QString failed;
            if (!syncTree(dirs.value(id), copyDir + "/" + id, &r, &failed))
                return fail(QStringLiteral("%1 could not be written.").arg(QDir::toNativeSeparators(failed)));
            r.modBytes += treeBytes(copyDir + "/" + id);
        }
        r.mods = ids;
        r.names = names;
        // the settings: the game folder's, without the computer's own keys (or the header alone: the console's defaults)
        QString settings = haveGame ? settingsText(m, gf.settingsFile()) : QString();
        if (settings.isEmpty() && !m->settingsHeader().isEmpty()) settings = m->settingsHeader() + "\n";
        const QString settingsFile = r.dataDir + "/" + (m->settingsFileName().isEmpty() ? QStringLiteral("ps2r.ini") : m->settingsFileName());
        if (!writeText(settingsFile, settings.toUtf8()))
            return fail(QStringLiteral("%1 could not be written.").arg(QDir::toNativeSeparators(settingsFile)));
        const QString discFile = r.dataDir + "/" + (m->discFileName().isEmpty() ? QStringLiteral("disc-image.txt") : m->discFileName());
        if (!writeText(discFile, (r.disc + "\n").toUtf8()))
            return fail(QStringLiteral("%1 could not be written.").arg(QDir::toNativeSeparators(discFile)));
    }
    const QString readmeFile = r.folder + "/" + (m->vitaReadme().isEmpty() ? QStringLiteral("README-vita.txt") : m->vitaReadme());
    if (!writeText(readmeFile, readme(m, r).toUtf8()))
        return fail(QStringLiteral("%1 could not be written.").arg(QDir::toNativeSeparators(readmeFile)));
    *report = r;
    return true;
}

}  // namespace ps2r::launcher
