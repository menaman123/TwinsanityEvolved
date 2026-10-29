// app/launcher/release_download.cpp -- the releases list and one checked asset.
#include "release_download.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QUrlQuery>
#include <functional>
#include <memory>
namespace ps2r::launcher {

namespace {
constexpr int kStallMs = 30000;     // a transfer that receives nothing for this long has stopped
constexpr int kMaxRedirects = 5;
constexpr qint64 kRateWindowMs = 3000;

QString token() {
    QString t = qEnvironmentVariable("GITHUB_TOKEN");   // lint: not-ui
    if (t.isEmpty()) t = qEnvironmentVariable("GH_TOKEN");   // lint: not-ui
    return t;
}

bool isRedirect(int status) { return status == 301 || status == 302 || status == 303 || status == 307 || status == 308; }

QString userAgent() {
    const QString v = QCoreApplication::applicationVersion();
    return QStringLiteral("TwinsanityEvolved-Launcher/%1").arg(v.isEmpty() ? QStringLiteral("0.2") : v);
}
}  // namespace

ReleaseDownload::ReleaseDownload(QObject* parent) : QObject(parent), net_(new QNetworkAccessManager(this)) {
    net_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

ReleaseDownload::~ReleaseDownload() {
    // quitting mid-transfer: the reply stops without the failure path running on a half-destroyed object
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
    if (part_.isOpen()) part_.close();
    if (!part_.fileName().isEmpty()) part_.remove();
}

bool ReleaseDownload::useToken() { return !token().isEmpty(); }

QNetworkReply* ReleaseDownload::get(const QUrl& url, bool json, bool auth) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setRawHeader("Accept", json ? "application/vnd.github+json" : "application/octet-stream");
    if (auth && useToken()) req.setRawHeader("Authorization", "Bearer " + token().toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(kStallMs);
    return net_->get(req);
}

void ReleaseDownload::fail(Failure failure) {
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
        reply_->deleteLater();
        reply_ = nullptr;
    }
    if (part_.isOpen()) part_.close();
    if (!part_.fileName().isEmpty()) part_.remove();
    part_.setFileName(QString());
    emit failed(int(failure));
}

void ReleaseDownload::cancel() {
    if (running()) fail(Cancelled);
}

void ReleaseDownload::fetchList(const QString& releasesUrl) {
    if (running()) return;
    QUrl url(releasesUrl);
    QUrlQuery q(url);
    if (url.scheme() != QLatin1String("file") && !q.hasQueryItem("per_page")) q.addQueryItem("per_page", "20");
    url.setQuery(q);
    redirects_ = 0;
    const QString host = url.host();
    // the list may be redirected (a renamed repository): followed by hand, the token kept for the API's own host
    auto start = std::make_shared<std::function<void(const QUrl&)>>();
    const std::weak_ptr<std::function<void(const QUrl&)>> weak = start;
    *start = [this, weak, host](const QUrl& at) {
        const auto self = weak.lock();
        if (!self) return;
        QNetworkReply* r = get(at, true, at.host() == host);
        reply_ = r;
        connect(r, &QNetworkReply::finished, this, [this, r, self] {
            if (reply_ != r) return;   // cancelled
            r->deleteLater();
            reply_ = nullptr;
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isRedirect(status) && redirects_++ < kMaxRedirects) {
                (*self)(r->url().resolved(r->header(QNetworkRequest::LocationHeader).toUrl()));
                return;
            }
            const QByteArray body = r->readAll();
            downloaded_ += body.size();
            if (r->error() != QNetworkReply::NoError || status >= 400) { emit failed(int(Offline)); return; }
            const QJsonDocument doc = QJsonDocument::fromJson(body);
            if (!doc.isArray()) { emit failed(int(Offline)); return; }
            emit listed(doc.array());
        });
    };
    (*start)(url);
}

void ReleaseDownload::noteRate() {
    const qint64 now = clock_.elapsed();
    samples_ << qMakePair(now, received_);
    while (samples_.size() > 2 && now - samples_.first().first > kRateWindowMs) samples_.removeFirst();
    const qint64 dt = now - samples_.first().first;
    rate_ = dt > 0 ? double(received_ - samples_.first().second) * 1000.0 / double(dt) : 0.0;
}

void ReleaseDownload::fetchChecked(const ReleaseAsset& asset, const ReleaseAsset& sums, const QString& dir) {
    if (running()) return;
    asset_ = asset;
    sums_ = sums;
    dir_ = QDir::cleanPath(dir);
    sumsText_.clear();
    received_ = 0;
    total_ = asset.bytes;
    rate_ = 0;
    follow(sums_.url, true, false);
}

// One request of the chain: SHA256SUMS (toFile false) or the asset (toFile true). A token goes with the first request of
// each only (the API's URL: `firstHop`); a redirect is followed without it.
void ReleaseDownload::follow(const QUrl& url, bool firstHop, bool toFile) {
    if (firstHop) {
        redirects_ = 0;
        if (toFile) {
            if (!QDir().mkpath(dir_)) { fail(NotWritable); return; }
            part_.close();
            part_.setFileName(dir_ + "/" + asset_.name + ".part");
            if (!part_.open(QIODevice::WriteOnly | QIODevice::Truncate)) { part_.setFileName(QString()); fail(NotWritable); return; }
            received_ = 0;
            samples_.clear();
            clock_.start();
            emit progress(0, total_, 0);
        }
    }
    QNetworkReply* r = get(url, false, firstHop);
    reply_ = r;
    if (toFile) {
        connect(r, &QNetworkReply::readyRead, this, [this, r] {
            if (reply_ != r) return;
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isRedirect(status) || status >= 400) return;   // a redirect's or an error's body is not the file
            const QByteArray chunk = r->readAll();
            if (part_.write(chunk) != chunk.size()) { fail(NotWritable); return; }
            received_ += chunk.size();
            downloaded_ += chunk.size();
            noteRate();
            emit progress(received_, total_, rate_);
        });
    }
    connect(r, &QNetworkReply::finished, this, [this, r, toFile] {
        if (reply_ != r) return;   // cancelled, or failed while reading
        r->deleteLater();
        reply_ = nullptr;
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (isRedirect(status) && redirects_++ < kMaxRedirects) {
            follow(r->url().resolved(r->header(QNetworkRequest::LocationHeader).toUrl()), false, toFile);
            return;
        }
        if (!toFile) {
            const QByteArray body = r->readAll();
            downloaded_ += body.size();
            if (r->error() != QNetworkReply::NoError || status >= 400) { fail(Offline); return; }
            sumsText_ = QString::fromUtf8(body);
            if (ReleaseCatalogue::shaFor(sumsText_, asset_.name).isEmpty()) { fail(Damaged); return; }   // SHA256SUMS does not name it
            follow(asset_.url, true, true);
            return;
        }
        const QByteArray rest = (status < 400 && !isRedirect(status)) ? r->readAll() : QByteArray();
        if (!rest.isEmpty()) {
            if (part_.write(rest) != rest.size()) { fail(NotWritable); return; }
            received_ += rest.size();
            downloaded_ += rest.size();
        }
        if (r->error() != QNetworkReply::NoError || status >= 400 || (total_ > 0 && received_ != total_)) { fail(Offline); return; }
        finishFile();
    });
}

void ReleaseDownload::finishFile() {
    part_.close();
    const QString file = dir_ + "/" + asset_.name;
    QFile::remove(file);
    if (!QFile::rename(part_.fileName(), file)) { fail(NotWritable); return; }
    part_.setFileName(QString());
    emit progress(received_, qMax(total_, received_), rate_);
    emit checking();
    const QString want = ReleaseCatalogue::shaFor(sumsText_, asset_.name);
    if (ReleaseCatalogue::sha256Of(file) != want) {
        QFile::remove(file);
        emit failed(int(Damaged));
        return;
    }
    emit fetched(file);
}

}  // namespace ps2r::launcher
