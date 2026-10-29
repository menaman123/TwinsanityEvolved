// app/launcher/disc_scanner.cpp -- the folder walk and the content identity, off the GUI thread.
#include "disc_scanner.h"
#include <QDateTime>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QMutexLocker>
#include <QStorageInfo>
#include <QtConcurrent/QtConcurrentRun>
#include "app/host/disc_identity.h"
#include "app/host/disc_image_hash.h"
namespace ps2r::launcher {

namespace {
// launcher.json "identities" rows of this shape (the whole-image verdict); a row of an older launcher is identified again
constexpr int kIdentitySchema = 2;

void walk(const QString& folder, int depth, int maxDepth, const QStringList& extensions, qint64 minBytes, QStringList* out) {
    QDir dir(folder);
    if (!dir.exists()) return;
    const QFileInfoList entries = dir.entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Readable, QDir::Name);
    for (const QFileInfo& fi : entries) {
        if (fi.isDir()) {
            if (!fi.isSymLink() && depth < maxDepth && !fi.isHidden() && !fi.fileName().endsWith(".app")) walk(fi.absoluteFilePath(), depth + 1, maxDepth, extensions, minBytes, out);
            continue;
        }
        const QString name = fi.fileName().toLower();
        for (const QString& ext : extensions) {
            if (name.endsWith(ext.toLower()) && fi.size() >= minBytes) { *out << fi.absoluteFilePath(); break; }
        }
    }
}

QString cacheKey(const QFileInfo& fi) { return fi.absoluteFilePath(); }
}  // namespace

DiscScanner::DiscScanner(QObject* parent) : QObject(parent) {}

DiscScanner::~DiscScanner() {
    cancel_ = true;
    futures_.waitForFinished();
}

QStringList DiscScanner::removableVolumes() {
    QStringList out;
    for (const QStorageInfo& v : QStorageInfo::mountedVolumes()) {
        if (!v.isValid() || !v.isReady() || v.isRoot()) continue;
        const QString root = v.rootPath();
#ifdef Q_OS_MACOS
        if (!root.startsWith("/Volumes/")) continue;
        if (v.device().startsWith("/dev/disk") && v.fileSystemType() == "apfs" && v.bytesTotal() > 500ll * 1024 * 1024 * 1024) continue;   // an internal volume
#elif defined(Q_OS_WIN)
        if (root.compare("C:/", Qt::CaseInsensitive) == 0) continue;
#else
        if (!(root.startsWith("/media/") || root.startsWith("/run/media/") || root.startsWith("/mnt/"))) continue;
#endif
        out << root;
    }
    return out;
}

QStringList DiscScanner::candidates(const QStringList& folders, int maxDepth, const QStringList& extensions, qint64 minBytes) {
    QStringList files;
    for (const QString& f : folders) walk(f, 0, maxDepth, extensions, minBytes, &files);
    files.removeDuplicates();
    return files;
}

DiscFound DiscScanner::identifyNow(const QString& path, bool hashImage, const std::function<bool(qint64, qint64)>& progress) {
    DiscFound d;
    const QFileInfo fi(path);
    d.path = fi.absoluteFilePath();
    d.sizeBytes = fi.size();
    d.modifiedMs = fi.lastModified().toMSecsSinceEpoch();
    ps2r::app::IdentifyOptions options;
    options.hash_image = hashImage;
    if (progress) options.progress = [&](uint64_t done, uint64_t total, const std::string&) { return progress(qint64(done), qint64(total)); };
    ps2r::app::DiscIdentity id;
    std::string error;
    d.accepted = ps2r::app::IdentifyDiscWith(QFile::encodeName(d.path).toStdString(), options, &id, &error);
    d.refusal = QString::fromStdString(error);
    d.region = QString::fromLatin1(ps2r::app::RegionName(id.region));
    d.serial = QString::fromStdString(id.serial);
    d.executable = QString::fromStdString(id.executable);
    d.volumeId = QString::fromStdString(id.volume_id);
    d.title = QString::fromStdString(id.title);
    d.known = id.known_program;
    d.retail = id.retail;
    if (id.list_retail)   // named only against a file list of retail provenance
        for (const std::vector<std::string>* list : {&id.differing_files, &id.added_files, &id.missing_files})
            for (const std::string& s : *list) d.differences << QString::fromStdString(s);
    d.warning = QString::fromStdString(id.warning);
    d.verdict = QString::fromLatin1(ps2r::app::VerdictName(id.verdict));
    d.checked = id.image_hashed;
    d.checking = d.accepted && !hashImage;   // the whole-image check follows
    d.imageBytes = qint64(id.image_bytes);
    d.dumpBytes = qint64(id.dump_bytes);
    d.dumpVersion = QString::fromStdString(id.dump_version);
    d.programDumps = int(id.program_dumps);
    if (id.image_hashed) {
        d.imageCrc32 = QString::fromStdString(ps2r::app::Crc32::Hex(id.image_crc32));
        d.imageSha1 = QString::fromStdString(id.image_sha1);
    }
    return d;
}

QJsonObject DiscScanner::toJson(const DiscFound& d) {
    QJsonObject o;
    o["identity_schema"] = kIdentitySchema;
    o["size"] = double(d.sizeBytes);
    o["modified"] = double(d.modifiedMs);
    o["accepted"] = d.accepted;
    o["refusal"] = d.refusal;
    o["region"] = d.region;
    o["serial"] = d.serial;
    o["executable"] = d.executable;
    o["volume"] = d.volumeId;
    o["title"] = d.title;
    o["known"] = d.known;
    o["retail"] = d.retail;
    o["differences"] = QJsonArray::fromStringList(d.differences);
    o["warning"] = d.warning;
    o["verdict"] = d.verdict;
    o["checked"] = d.checked;
    o["image_bytes"] = double(d.imageBytes);
    o["dump_bytes"] = double(d.dumpBytes);
    o["dump_version"] = d.dumpVersion;
    o["program_dumps"] = d.programDumps;
    o["image_crc32"] = d.imageCrc32;
    o["image_sha1"] = d.imageSha1;
    return o;
}

DiscFound DiscScanner::fromJson(const QJsonObject& o) {
    DiscFound d;
    d.sizeBytes = qint64(o.value("size").toDouble());
    d.modifiedMs = qint64(o.value("modified").toDouble());
    d.accepted = o.value("accepted").toBool();
    d.refusal = o.value("refusal").toString();
    d.region = o.value("region").toString();
    d.serial = o.value("serial").toString();
    d.executable = o.value("executable").toString();
    d.volumeId = o.value("volume").toString();
    d.title = o.value("title").toString();
    d.known = o.value("known").toBool();
    d.retail = o.value("retail").toBool();
    for (const QJsonValue& v : o.value("differences").toArray()) d.differences << v.toString();
    d.warning = o.value("warning").toString();
    d.verdict = o.value("verdict").toString();
    d.checked = o.value("checked").toBool();
    d.imageBytes = qint64(o.value("image_bytes").toDouble());
    d.dumpBytes = qint64(o.value("dump_bytes").toDouble());
    d.dumpVersion = o.value("dump_version").toString();
    d.programDumps = o.value("program_dumps").toInt();
    d.imageCrc32 = o.value("image_crc32").toString();
    d.imageSha1 = o.value("image_sha1").toString();
    if (stale(o)) {   // an older launcher's identity: "retail" there meant a file list matched -- not a verdict now
        d.retail = false;
        d.verdict.clear();
        d.differences.clear();
    }
    return d;
}

bool DiscScanner::stale(const QJsonObject& o) { return o.value("identity_schema").toInt() < kIdentitySchema || !o.contains("verdict"); }

QJsonObject DiscScanner::cache() const {
    QMutexLocker lock(&mutex_);
    return cache_;
}

void DiscScanner::setCache(const QJsonObject& cache) {
    QMutexLocker lock(&mutex_);
    cache_ = cache;
}

DiscFound DiscScanner::identifyCached(const QString& path, bool* needsHash) {
    *needsHash = false;
    const QFileInfo fi(path);
    {
        QMutexLocker lock(&mutex_);
        const QJsonObject c = cache_.value(cacheKey(fi)).toObject();
        if (!c.isEmpty() && !stale(c) && qint64(c.value("size").toDouble()) == fi.size() &&
            qint64(c.value("modified").toDouble()) == fi.lastModified().toMSecsSinceEpoch()) {
            DiscFound d = fromJson(c);
            d.path = fi.absoluteFilePath();
            return d;   // a refusal or a checked verdict: nothing to read again
        }
    }
    DiscFound d = identifyNow(path, false);
    *needsHash = d.accepted;
    if (!d.accepted && !cancel_ && d.refusal != QLatin1String("The check was stopped.")) {
        QMutexLocker lock(&mutex_);
        cache_.insert(cacheKey(fi), toJson(d));
    }
    return d;
}

DiscFound DiscScanner::hashCached(const QString& path) {
    const QFileInfo fi(path);
    qint64 reported = -1;
    DiscFound d = identifyNow(path, true, [&](qint64 done, qint64 total) {
        if (cancel_) return false;
        if (reported < 0 || done - reported >= (16ll << 20) || done == total) {
            reported = done;
            QMetaObject::invokeMethod(this, [this, path, done, total] { emit checking(path, done, total); }, Qt::QueuedConnection);
        }
        return true;
    });
    if (!cancel_ && d.refusal != QLatin1String("The check was stopped.")) {
        QMutexLocker lock(&mutex_);
        cache_.insert(cacheKey(fi), toJson(d));
    }
    return d;
}

void DiscScanner::start(const QStringList& folders, bool removable, int maxDepth, const QStringList& extensions, qint64 minBytes) {
    QStringList all = folders;
    if (removable) all += removableVolumes();
    all.removeDuplicates();
    cancel_ = false;
    ++tasks_;
    futures_.addFuture(QtConcurrent::run([=] {
        const QStringList files = candidates(all, maxDepth, extensions, minBytes);
        int foundCount = 0;
        QStringList toHash;
        // first every image's quick identity (the cards appear), then the whole-image checks one after another
        for (const QString& f : files) {
            if (cancel_) break;
            QMetaObject::invokeMethod(this, [this, f] { emit checking(f, 0, 0); }, Qt::QueuedConnection);
            bool needsHash = false;
            DiscFound d = identifyCached(f, &needsHash);
            if (!d.accepted) continue;   // another game, or not a disc: a search does not complain about it
            d.fromSearch = true;
            ++foundCount;
            if (needsHash) toHash << f;
            QMetaObject::invokeMethod(this, [this, d] { emit found(d); }, Qt::QueuedConnection);
        }
        for (const QString& f : toHash) {
            if (cancel_) break;
            DiscFound d = hashCached(f);
            if (cancel_ || !d.accepted) continue;
            d.fromSearch = true;
            QMetaObject::invokeMethod(this, [this, d] { emit checked(d); }, Qt::QueuedConnection);
        }
        const int looked = int(files.size());
        QMetaObject::invokeMethod(this, [this, foundCount, looked] {
            --tasks_;
            emit finished(foundCount, looked);
        }, Qt::QueuedConnection);
    }));
}

void DiscScanner::identify(const QString& path) {
    cancel_ = false;
    ++tasks_;
    futures_.addFuture(QtConcurrent::run([=] {
        QMetaObject::invokeMethod(this, [this, path] { emit checking(path, 0, 0); }, Qt::QueuedConnection);
        bool needsHash = false;
        const DiscFound d = identifyCached(path, &needsHash);
        const bool hashing = needsHash && !cancel_;
        // the task ends with the last signal: a page asking running() in its handler sees the scanner idle
        QMetaObject::invokeMethod(this, [this, d, hashing] {
            if (!hashing) --tasks_;
            if (d.accepted) emit found(d);
            else emit refused(d);
        }, Qt::QueuedConnection);
        if (!hashing) return;
        const DiscFound c = hashCached(path);
        const bool ok = !cancel_ && c.accepted;
        QMetaObject::invokeMethod(this, [this, c, ok] {
            --tasks_;
            if (ok) emit checked(c);
        }, Qt::QueuedConnection);
    }));
}

}  // namespace ps2r::launcher
