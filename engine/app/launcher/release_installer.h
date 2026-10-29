// app/launcher/release_installer.h -- Install: the finished game from the title's releases, never compiled (D-A). One
// install = look up the newest release that carries the target's asset and SHA256SUMS (launcher.json releases_url, else
// the manifest's), download the asset into Downloads/ with progress and rate (Qt Network, redirects followed by hand
// so a token never travels to the storage host, a 30 s stall timeout), compare its sha256 with SHA256SUMS, then place
// it: a computer's zip unpacked (Qt's zip reader, every entry checked to stay inside) and copied into the game folder
// without touching the user's data, the PS Vita's .vpk copied in. Nothing reaches the folder before its checksum
// matched. The same lookup answers "is there a newer release" for Update.
#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QObject>
#include <QStringList>
#include <QPointer>
#include <QUrl>
#include <QVector>
#include "release_catalogue.h"
class QNetworkAccessManager;
class QNetworkReply;
namespace ps2r::launcher {
class AppData;
class TitleManifest;

class ReleaseInstaller : public QObject {
    Q_OBJECT
public:
    enum Phase { Idle, Looking, Downloading, Checking, Placing, Done, Failed };
    enum Failure { NoFailure, Offline, NoRelease, Damaged, NotWritable };
    ReleaseInstaller(AppData* appData, const TitleManifest* manifest, QObject* parent = nullptr);
    ~ReleaseInstaller() override;

    QString releasesUrl() const;   // launcher.json releases_url, else the manifest's
    // target: a computer's id (macos-arm64 ...) or "vita"; folder: the game folder (made when the game is placed)
    void install(const QString& target, const QString& folder);
    void retry();
    void cancel();
    // One lookup: latestFound(target, tag) when a release carries the target; silent when offline.
    void checkLatest(const QString& target);

    bool running() const { return phase_ == Looking || phase_ == Downloading || phase_ == Checking || phase_ == Placing; }
    Phase phase() const { return phase_; }
    Failure failure() const { return failure_; }
    QString failureText() const { return failureText_; }
    QString target() const { return target_; }
    QString folder() const { return folder_; }
    QString tag() const { return choice_.tag; }
    qint64 received() const { return received_; }
    qint64 total() const { return total_; }
    double rate() const { return rate_; }
    qint64 downloadedBytes() const { return downloaded_; }   // received over the network this session (the judge's figure)
    QString placedFile() const { return placed_; }           // the executable (a computer) or the .vpk
    QStringList bundledMods() const { return bundledMods_; }   // the release's mods placed into the mods folder this time (new ids only)
    static QString userAgent();

signals:
    void phaseChanged(int phase);
    void progress(qint64 received, qint64 total, double bytesPerSecond);
    void finished(bool ok);
    void latestFound(const QString& target, const QString& tag);

private:
    QNetworkReply* get(const QUrl& url, bool json, bool auth);
    void setPhase(Phase phase);
    void fail(Failure failure);
    void lookUp();
    void fetchSums();
    void fetchAsset();
    void verifyAndPlace();
    bool placeAsset(const QString& file, QString* error);
    bool unpack(const QString& zip, const QString& into, QString* error);
    bool copyInto(const QString& from, const QString& to, bool top, QString* failed);
    bool placeBundledMods(const QString& from, QString* failed);   // the release's mods/<id> into the game folder's mods folder, new ids only
    QString assetPattern(const QString& target) const;
    bool useToken() const;
    void noteRate();

    AppData* appData_;
    const TitleManifest* manifest_;
    QNetworkAccessManager* net_;
    QPointer<QNetworkReply> reply_;
    QFile part_;
    Phase phase_ = Idle;
    Failure failure_ = NoFailure;
    QString failureText_, target_, folder_, sumsText_, placed_;
    QStringList bundledMods_;   // the ids placeBundledMods added this time
    ReleaseChoice choice_;
    qint64 received_ = 0, total_ = 0, downloaded_ = 0;
    double rate_ = 0;
    QElapsedTimer clock_;
    QVector<QPair<qint64, qint64>> samples_;   // (ms, bytes) of the last seconds, for the rate
    int redirects_ = 0;
};

}  // namespace ps2r::launcher
