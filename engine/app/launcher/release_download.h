// app/launcher/release_download.h -- the network half of the launcher's own update: the releases list, then ONE asset
// checked against the release's SHA256SUMS (the game's install has the same steps inside ReleaseInstaller, which is
// left as it is). Qt Network end to end: the API's JSON with a User-Agent and, when GITHUB_TOKEN / GH_TOKEN is
// set, a token -- sent to the first host only: redirects are followed by hand and the storage host never sees it; a
// 30 s stall timeout; the asset written to <dir>/<name>.part, renamed once complete, then its sha256 compared with its
// SHA256SUMS line (a mismatch deletes it). Nothing else may use the file before fetched(). One chain at a time; file://
// lists and assets work the same (the hand-test kit's folder).
#pragma once
#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QObject>
#include <QPointer>
#include <QVector>
#include "release_catalogue.h"
class QNetworkAccessManager;
class QNetworkReply;
class QNetworkRequest;
namespace ps2r::launcher {

class ReleaseDownload : public QObject {
    Q_OBJECT
public:
    enum Failure { NoFailure, Offline, Damaged, NotWritable, Cancelled };
    explicit ReleaseDownload(QObject* parent = nullptr);
    ~ReleaseDownload() override;

    // GET <releasesUrl>?per_page=20 -> listed(the array, newest first) or failed(Offline)
    void fetchList(const QString& releasesUrl);
    // SHA256SUMS, then the asset into <dir>/<asset name> -> fetched(path) once its sha256 matched, or failed(why)
    void fetchChecked(const ReleaseAsset& asset, const ReleaseAsset& sums, const QString& dir);
    void cancel();   // failed(Cancelled); a partial file is removed

    bool running() const { return reply_ != nullptr; }
    qint64 received() const { return received_; }
    qint64 total() const { return total_; }
    double rate() const { return rate_; }
    qint64 downloadedBytes() const { return downloaded_; }   // received over the network this session
    static bool useToken();

signals:
    void listed(const QJsonArray& releases);
    void progress(qint64 received, qint64 total, double bytesPerSecond);
    void checking();                     // the asset is complete; its sha256 is being compared
    void fetched(const QString& path);   // the checked file
    void failed(int failure);

private:
    QNetworkReply* get(const QUrl& url, bool json, bool auth);
    void follow(const QUrl& url, bool firstHop, bool toFile);
    void finishFile();
    void fail(Failure failure);
    void noteRate();

    QNetworkAccessManager* net_;
    QPointer<QNetworkReply> reply_;
    QFile part_;
    ReleaseAsset asset_, sums_;
    QString dir_, sumsText_;
    qint64 received_ = 0, total_ = 0, downloaded_ = 0;
    double rate_ = 0;
    int redirects_ = 0;
    QElapsedTimer clock_;
    QVector<QPair<qint64, qint64>> samples_;
};

}  // namespace ps2r::launcher
