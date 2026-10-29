// app/launcher/release_installer.cpp -- Install, step by step.
#include "release_installer.h"
#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <functional>
#include <memory>
#include <QUrlQuery>
#include "app_data.h"
#include "game_folder.h"
#include "title_manifest.h"
#include "zip_unpack.h"
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
}  // namespace

ReleaseInstaller::ReleaseInstaller(AppData* appData, const TitleManifest* manifest, QObject* parent)
    : QObject(parent), appData_(appData), manifest_(manifest), net_(new QNetworkAccessManager(this)) {
    net_->setRedirectPolicy(QNetworkRequest::ManualRedirectPolicy);
}

ReleaseInstaller::~ReleaseInstaller() {
    // quitting during a download: the transfer stops without its failure path running on a half-destroyed object
    if (reply_) {
        reply_->disconnect(this);
        reply_->abort();
    }
}

QString ReleaseInstaller::userAgent() {
    return QStringLiteral("TwinsanityEvolved-Launcher/%1").arg(QCoreApplication::applicationVersion().isEmpty() ? QStringLiteral("0.2") : QCoreApplication::applicationVersion());
}

QString ReleaseInstaller::releasesUrl() const {
    const QString chosen = appData_->value("releases_url").toString();
    return chosen.isEmpty() ? manifest_->releasesUrl() : chosen;
}

bool ReleaseInstaller::useToken() const { return !token().isEmpty(); }

QString ReleaseInstaller::assetPattern(const QString& target) const {
    return target == QLatin1String("vita") ? manifest_->vitaAssetPattern() : manifest_->zipAssetPattern();
}

QNetworkReply* ReleaseInstaller::get(const QUrl& url, bool json, bool auth) {
    QNetworkRequest req(url);
    req.setHeader(QNetworkRequest::UserAgentHeader, userAgent());
    req.setRawHeader("Accept", json ? "application/vnd.github+json" : "application/octet-stream");
    if (auth && useToken()) req.setRawHeader("Authorization", "Bearer " + token().toUtf8());
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    req.setTransferTimeout(kStallMs);
    return net_->get(req);
}

void ReleaseInstaller::setPhase(Phase phase) {
    phase_ = phase;
    emit phaseChanged(int(phase));
}

void ReleaseInstaller::fail(Failure failure) {
    if (part_.isOpen()) part_.close();
    if (!part_.fileName().isEmpty()) part_.remove();
    failure_ = failure;
    switch (failure) {
    case Offline: failureText_ = QStringLiteral("The download did not finish. Check your connection and try again."); break;
    case NoRelease:
        failureText_ = QStringLiteral("There is no download for %1 yet. Try again later, or go back and choose another place to play.")
                           .arg(manifest_->targetLabel(target_));
        break;
    case Damaged: failureText_ = QStringLiteral("The download was damaged and has been deleted. Try again."); break;
    case NotWritable:
        failureText_ = QStringLiteral("The game could not be put in %1. If the game is open, close it and try again.")
                           .arg(AppData::shortPath(folder_));
        break;
    case NoFailure: failureText_.clear(); break;
    }
    setPhase(Failed);
    emit finished(false);
}

void ReleaseInstaller::install(const QString& target, const QString& folder) {
    if (running()) return;
    target_ = target;
    folder_ = QDir::cleanPath(folder);
    failure_ = NoFailure;
    failureText_.clear();
    placed_.clear();
    choice_ = ReleaseChoice{};
    received_ = total_ = 0;
    rate_ = 0;
    lookUp();
}

void ReleaseInstaller::retry() {
    if (running() || target_.isEmpty()) return;
    install(target_, folder_);
}

void ReleaseInstaller::cancel() {
    if (reply_) reply_->abort();
}

void ReleaseInstaller::lookUp() {
    setPhase(Looking);
    QUrl url(releasesUrl());
    QUrlQuery q(url);
    if (!q.hasQueryItem("per_page")) q.addQueryItem("per_page", "20");
    url.setQuery(q);
    redirects_ = 0;
    QNetworkReply* r = get(url, true, true);
    reply_ = r;
    connect(r, &QNetworkReply::finished, this, [this, r] {
        r->deleteLater();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = r->readAll();
        downloaded_ += body.size();
        if (r->error() != QNetworkReply::NoError || status >= 400) { fail(Offline); return; }
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        if (!doc.isArray()) { fail(Offline); return; }
        if (!ReleaseCatalogue::pick(doc.array(), assetPattern(target_), target_, manifest_->sumsAsset(), useToken(), &choice_)) {
            fail(NoRelease);
            return;
        }
        total_ = choice_.asset.bytes;
        fetchSums();
    });
}

void ReleaseInstaller::fetchSums() {
    sumsText_.clear();
    redirects_ = 0;
    // the request follows its redirects by hand (the token is never sent past the first hop); the closure holds itself
    // weakly and each request in flight holds it strongly, so it lives exactly as long as the chain
    auto start = std::make_shared<std::function<void(const QUrl&, bool)>>();
    const std::weak_ptr<std::function<void(const QUrl&, bool)>> weak = start;
    *start = [this, weak](const QUrl& url, bool auth) {
        const auto self = weak.lock();
        if (!self) return;
        QNetworkReply* r = get(url, false, auth);
        reply_ = r;
        connect(r, &QNetworkReply::finished, this, [this, r, self] {
            r->deleteLater();
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isRedirect(status) && redirects_++ < kMaxRedirects) {
                (*self)(r->url().resolved(r->header(QNetworkRequest::LocationHeader).toUrl()), false);   // no token to the storage host
                return;
            }
            const QByteArray body = r->readAll();
            downloaded_ += body.size();
            if (r->error() != QNetworkReply::NoError || status >= 400) { fail(Offline); return; }
            sumsText_ = QString::fromUtf8(body);
            if (ReleaseCatalogue::shaFor(sumsText_, choice_.asset.name).isEmpty()) { fail(Damaged); return; }
            fetchAsset();
        });
    };
    (*start)(choice_.sums.url, true);
}

void ReleaseInstaller::noteRate() {
    const qint64 now = clock_.elapsed();
    samples_ << qMakePair(now, received_);
    while (samples_.size() > 2 && now - samples_.first().first > kRateWindowMs) samples_.removeFirst();
    const qint64 dt = now - samples_.first().first;
    rate_ = dt > 0 ? double(received_ - samples_.first().second) * 1000.0 / double(dt) : 0.0;
}

void ReleaseInstaller::fetchAsset() {
    setPhase(Downloading);
    QDir().mkpath(appData_->downloadsDir());
    part_.close();
    part_.setFileName(appData_->downloadsDir() + "/" + choice_.asset.name + ".part");
    if (!part_.open(QIODevice::WriteOnly | QIODevice::Truncate)) { fail(NotWritable); return; }
    received_ = 0;
    samples_.clear();
    clock_.start();
    redirects_ = 0;
    emit progress(0, total_, 0);
    auto start = std::make_shared<std::function<void(const QUrl&, bool)>>();
    const std::weak_ptr<std::function<void(const QUrl&, bool)>> weak = start;
    *start = [this, weak](const QUrl& url, bool auth) {
        const auto self = weak.lock();
        if (!self) return;
        QNetworkReply* r = get(url, false, auth);
        reply_ = r;
        connect(r, &QNetworkReply::readyRead, this, [this, r] {
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isRedirect(status) || status >= 400) return;   // a redirect's or an error's body is not the file
            const QByteArray chunk = r->readAll();
            if (part_.write(chunk) != chunk.size()) { r->abort(); return; }
            received_ += chunk.size();
            downloaded_ += chunk.size();
            noteRate();
            emit progress(received_, total_, rate_);
        });
        connect(r, &QNetworkReply::finished, this, [this, r, self] {
            r->deleteLater();
            const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            if (isRedirect(status) && redirects_++ < kMaxRedirects) {
                (*self)(r->url().resolved(r->header(QNetworkRequest::LocationHeader).toUrl()), false);
                return;
            }
            const QByteArray rest = (status < 400 && !isRedirect(status)) ? r->readAll() : QByteArray();
            if (!rest.isEmpty()) { part_.write(rest); received_ += rest.size(); downloaded_ += rest.size(); }
            if (r->error() != QNetworkReply::NoError || status >= 400 || (total_ > 0 && received_ != total_)) { fail(Offline); return; }
            part_.close();
            verifyAndPlace();
        });
    };
    (*start)(choice_.asset.url, true);
}

void ReleaseInstaller::verifyAndPlace() {
    setPhase(Checking);
    const QString file = appData_->downloadsDir() + "/" + choice_.asset.name;
    QFile::remove(file);
    if (!QFile::rename(part_.fileName(), file)) { fail(NotWritable); return; }
    const QString want = ReleaseCatalogue::shaFor(sumsText_, choice_.asset.name);
    if (ReleaseCatalogue::sha256Of(file) != want) {
        QFile::remove(file);
        fail(Damaged);
        return;
    }
    setPhase(Placing);
    QString error;
    const bool ok = placeAsset(file, &error);
    QFile::remove(file);   // the folder is the copy that is kept
    if (!ok) { fail(NotWritable); return; }
    setPhase(Done);
    emit finished(true);
}

bool ReleaseInstaller::placeAsset(const QString& file, QString* error) {
    if (!QDir().mkpath(folder_)) return false;
    if (target_ == QLatin1String("vita")) {
        const QString dest = folder_ + "/" + manifest_->vitaFile();
        QFile::remove(dest);
        if (!QFile::copy(file, dest)) return false;
        placed_ = dest;
        return true;
    }
    const ComputerTarget* computer = manifest_->computer(target_);
    const QString exe = computer ? computer->exe : manifest_->binaryName();
    const QString tmp = appData_->downloadsDir() + "/unpack-" + choice_.tag;
    QDir(tmp).removeRecursively();
    if (!unpack(file, tmp, error)) { QDir(tmp).removeRecursively(); return false; }
    // the zip holds one folder (twinsanityevolved-<target>/) with the game in it
    QString root = tmp;
    const QStringList dirs = QDir(tmp).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!QFileInfo::exists(tmp + "/" + exe) && dirs.size() == 1) root = tmp + "/" + dirs.first();
    if (!QFileInfo::exists(root + "/" + exe)) {
        if (error) *error = QStringLiteral("the download holds no %1").arg(exe);
        QDir(tmp).removeRecursively();
        return false;
    }
    QString failed;
    const bool copied = copyInto(root, folder_, true, &failed);
    // the release's own mods (mods/<id>/: the Arabic language pack -- the port's translation and glyphs, never disc
    // data) go into the game folder's mods folder when that id is not there yet: a mod the player removed or changed is
    // left alone (the player's data is never overwritten by a release); a language pack is offered as a text
    // language as soon as it is there, a mod is off until switched on in the launcher's Mods page
    const bool placedMods = copied && placeBundledMods(root + "/" + manifest_->modsName(), &failed);
    QDir(tmp).removeRecursively();
    if (!copied || !placedMods) {
        if (error) *error = QStringLiteral("%1 could not be written").arg(failed);
        return false;
    }
    placed_ = folder_ + "/" + exe;
    QFile(placed_).setPermissions(QFile(placed_).permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser | QFileDevice::ExeGroup);
    return true;
}

bool ReleaseInstaller::unpack(const QString& zipPath, const QString& into, QString* error) {
    return unpackZip(zipPath, into, error);   // every entry checked to stay inside `into` (zip_unpack.h)
}

bool ReleaseInstaller::copyInto(const QString& from, const QString& to, bool top, QString* failed) {
    if (!QDir().mkpath(to)) { *failed = to; return false; }
    for (const QFileInfo& fi : QDir(from).entryInfoList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden | QDir::System)) {
        // the user's data is never written from a release: settings, saves, mods, the disc's name
        if (top && manifest_->protectedNames().contains(fi.fileName(), Qt::CaseInsensitive)) continue;
        const QString dest = to + "/" + fi.fileName();
        if (fi.isDir() && !fi.isSymLink()) {
            if (!copyInto(fi.absoluteFilePath(), dest, false, failed)) return false;
            continue;
        }
        QFile::remove(dest);
        if (fi.isSymLink()) {
            if (!QFile::link(fi.symLinkTarget(), dest)) { *failed = dest; return false; }
            continue;
        }
        if (!QFile::copy(fi.absoluteFilePath(), dest)) { *failed = dest; return false; }
        QFile(dest).setPermissions(fi.permissions());
    }
    return true;
}

void ReleaseInstaller::checkLatest(const QString& target) {
    QUrl url(releasesUrl());
    QUrlQuery q(url);
    if (!q.hasQueryItem("per_page")) q.addQueryItem("per_page", "20");
    url.setQuery(q);
    QNetworkReply* r = get(url, true, true);
    connect(r, &QNetworkReply::finished, this, [this, r, target] {
        r->deleteLater();
        const QByteArray body = r->readAll();
        downloaded_ += body.size();
        const int status = r->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (r->error() != QNetworkReply::NoError || status >= 400) return;   // offline: no update offered this time
        const QJsonDocument doc = QJsonDocument::fromJson(body);
        ReleaseChoice c;
        if (doc.isArray() && ReleaseCatalogue::pick(doc.array(), assetPattern(target), target, manifest_->sumsAsset(), useToken(), &c))
            emit latestFound(target, c.tag);
    });
}

bool ReleaseInstaller::placeBundledMods(const QString& from, QString* failed) {
    if (!QFileInfo(from).isDir()) return true;   // a release without mods/
    const QString modsDir = GameFolder(manifest_, folder_).folderSetting("paths.mods", manifest_->modsName());
    for (const QFileInfo& fi : QDir(from).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        if (!QFileInfo::exists(fi.absoluteFilePath() + "/mod.json")) continue;
        const QString dest = modsDir + "/" + fi.fileName();
        if (QFileInfo::exists(dest)) continue;   // the player's copy stays as it is
        if (!copyInto(fi.absoluteFilePath(), dest, false, failed)) return false;
        bundledMods_ << fi.fileName();
    }
    return true;
}

}  // namespace ps2r::launcher
