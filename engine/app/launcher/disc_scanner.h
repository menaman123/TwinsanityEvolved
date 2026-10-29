// app/launcher/disc_scanner.h -- finds disc images in the usual places (the manifest's folders, removable drives) and
// identifies each by its content on a worker thread (engine/app/host/disc_identity.h: the program, the region, and
// whether the whole image is a known retail dump). Two steps: the quick identity (the directories and the size, a few
// milliseconds) arrives as found() with `checking` set while the image still has to be hashed; the hash (CRC-32 + SHA-1 of
// the whole image, seconds for a DVD) arrives as checked() with the verdict. Results arrive as signals on the GUI thread
// and are cached by path, size and modification time (launcher.json "identities"), so a known image is not read again.
#pragma once
#include <QFutureSynchronizer>
#include <QJsonObject>
#include <QMutex>
#include <QObject>
#include <QStringList>
#include <atomic>
#include <functional>
namespace ps2r::launcher {

struct DiscFound {
    QString path;
    qint64 sizeBytes = 0;
    qint64 modifiedMs = 0;     // with the size, the cache key
    bool accepted = false;     // retail, modified or unknown but plausible
    QString refusal;           // the identity's sentence when refused
    QString region;            // PAL / NTSC-U / NTSC-J / unknown region
    QString serial;            // SLES-52568
    QString executable;        // SLES_525.68
    QString volumeId;
    QString title;             // the title's name, when the tables know the program
    bool known = false;        // the program is a known release (the table of known dumps has it)
    bool retail = false;       // the whole image is a known retail dump (size, CRC-32 and SHA-1)
    QStringList differences;   // the files that differ -- named only against a file list of retail provenance
    QString warning;           // accepted with a doubt
    bool fromSearch = false;   // found by a search of the usual folders (not chosen or dropped by the user)
    // the whole image against the table of known dumps
    QString verdict;           // "retail" | "retail-size" | "modified" | "unknown" (disc_identity.h VerdictName)
    bool checking = false;     // the image is still being hashed (the verdict may still change)
    bool checked = false;      // the image was hashed (imageSha1 / imageCrc32 set)
    qint64 imageBytes = 0;     // the bytes compared (the logical image)
    qint64 dumpBytes = 0;      // the known dump it was compared with (the match, else the closest)
    QString dumpVersion;       // that dump's version ("1.01")
    int programDumps = 0;      // dumps of this program in the table
    QString imageCrc32, imageSha1;
};

class DiscScanner : public QObject {
    Q_OBJECT
public:
    explicit DiscScanner(QObject* parent = nullptr);
    ~DiscScanner() override;

    // The candidates under `folders` (to `maxDepth`; `.iso`/`.bin` of at least `minBytes`) and, when `removable`, every
    // mounted removable drive; each identified on a worker. found() for every accepted image (refused ones are skipped),
    // then checked() for each one whose image had to be hashed.
    void start(const QStringList& folders, bool removable, int maxDepth, const QStringList& extensions, qint64 minBytes);
    // One file the user chose or dropped: found() or refused(), then checked() when its image was hashed.
    void identify(const QString& path);
    bool running() const { return tasks_.load() > 0; }
    void cancel() { cancel_ = true; }

    QJsonObject cache() const;
    void setCache(const QJsonObject& cache);

    static QStringList removableVolumes();
    static QStringList candidates(const QStringList& folders, int maxDepth, const QStringList& extensions, qint64 minBytes);
    // Synchronous: the identity of one file (the worker's body). `hashImage` = the whole-image check (the final verdict);
    // without it the quick identity (`checking` set when the verdict needs the hash). `progress(done, total)` false stops it.
    static DiscFound identifyNow(const QString& path, bool hashImage = true, const std::function<bool(qint64, qint64)>& progress = {});
    static QJsonObject toJson(const DiscFound& d);
    static DiscFound fromJson(const QJsonObject& o);
    // A cached identity of an older launcher (no whole-image verdict): it must be identified again.
    static bool stale(const QJsonObject& o);

signals:
    void checking(const QString& path, qint64 done, qint64 total);
    void found(const DiscFound& disc);
    void refused(const DiscFound& disc);
    void checked(const DiscFound& disc);
    void finished(int foundCount, int looked);

private:
    // the cached verdict, or the quick identity (`*needsHash` when the image still has to be hashed)
    DiscFound identifyCached(const QString& path, bool* needsHash);
    DiscFound hashCached(const QString& path);
    QFutureSynchronizer<void> futures_;
    std::atomic<int> tasks_{0};
    std::atomic<bool> cancel_{false};
    mutable QMutex mutex_;
    QJsonObject cache_;
};

}  // namespace ps2r::launcher
