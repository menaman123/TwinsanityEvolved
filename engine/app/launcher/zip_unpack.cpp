// app/launcher/zip_unpack.cpp -- see zip_unpack.h.
#include "zip_unpack.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSet>
#include <QVector>
#if __has_include(<QtCore/private/qzipreader_p.h>)
#include <QtCore/private/qzipreader_p.h>
#else
#include <QtGui/private/qzipreader_p.h>
#endif
namespace ps2r::launcher {

bool zipEntryIsSafe(const QString& path) {
    if (path.isEmpty() || path.startsWith('/') || (path.size() > 1 && path[1] == ':')) return false;
    for (const QString& part : path.split('/', Qt::SkipEmptyParts))
        if (part == QLatin1String("..")) return false;
    return true;
}

bool unpackZip(const QString& zipPath, const QString& into, QString* error) {
    QZipReader zip(zipPath);
    if (!zip.isReadable() || zip.status() != QZipReader::NoError) {
        if (error) *error = QStringLiteral("the download is not a readable zip");
        return false;
    }
    if (!QDir().mkpath(into)) {
        if (error) *error = QStringLiteral("%1 could not be made").arg(QDir::toNativeSeparators(into));
        return false;
    }
    // A zip written on Windows (PowerShell's Compress-Archive) gives its folder entries no directory attribute, and Qt's
    // reader then names them files (their trailing '/' dropped): an entry that holds other entries is a folder
    const QVector<QZipReader::FileInfo> entries = zip.fileInfoList();
    QSet<QString> folders;
    for (const QZipReader::FileInfo& fi : entries) {
        QString path = fi.filePath;
        path.replace('\\', '/');
        for (qsizetype at = path.indexOf('/'); at > 0; at = path.indexOf('/', at + 1)) folders.insert(path.left(at));
    }
    for (const QZipReader::FileInfo& fi : entries) {
        QString path = fi.filePath;
        path.replace('\\', '/');
        while (path.endsWith('/')) path.chop(1);
        if (!zipEntryIsSafe(path)) {
            if (error) *error = QStringLiteral("the download names a file outside its folder (%1)").arg(path);
            return false;
        }
        const QString dest = into + "/" + path;
        auto failed = [&](const QString& what) {
            if (error) *error = QStringLiteral("%1 could not be written").arg(QDir::toNativeSeparators(what));
            return false;
        };
        if (fi.isDir || folders.contains(path)) {
            if (!QDir().mkpath(dest)) return failed(dest);
            continue;
        }
        if (!QDir().mkpath(QFileInfo(dest).absolutePath())) return failed(QFileInfo(dest).absolutePath());
        if (fi.isSymLink) {
            const QString link = QFile::decodeName(zip.fileData(fi.filePath));
            if (!zipEntryIsSafe(QFileInfo(path).path() + "/" + link) || !QFile::link(link, dest)) return failed(dest);
            continue;
        }
        QFile out(dest);
        if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate)) return failed(dest);
        const QByteArray data = zip.fileData(fi.filePath);
        if (out.write(data) != data.size()) return failed(dest);
        out.close();
        if (fi.permissions) out.setPermissions(fi.permissions | QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    }
    if (zip.status() != QZipReader::NoError) {
        if (error) *error = QStringLiteral("the download is not a readable zip");
        return false;
    }
    return true;
}

}  // namespace ps2r::launcher
