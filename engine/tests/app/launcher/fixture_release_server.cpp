// tests/app/launcher/fixture_release_server.cpp -- the releases stand-in.
#include "fixture_release_server.h"
#include <QCryptographicHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <memory>
namespace ps2r::launcher {

FixtureReleaseServer::FixtureReleaseServer(QObject* parent) : QObject(parent), server_(new QTcpServer(this)) {
    connect(server_, &QTcpServer::newConnection, this, &FixtureReleaseServer::onConnection);
}

bool FixtureReleaseServer::start() {
    if (!server_->listen(QHostAddress::LocalHost, port_)) return false;
    port_ = server_->serverPort();
    return true;
}

void FixtureReleaseServer::stop() { server_->close(); }

QString FixtureReleaseServer::releasesUrl() const { return QStringLiteral("http://127.0.0.1:%1/releases").arg(port_); }

void FixtureReleaseServer::clear() {
    releases_.clear();
    writeList();
}

void FixtureReleaseServer::publish(const QString& tag, const QHash<QString, QByteArray>& assets) {
    releases_.clear();
    addRelease(tag, assets);
}

void FixtureReleaseServer::addRelease(const QString& tag, const QHash<QString, QByteArray>& assets, const QDateTime& published, bool prerelease) {
    Release r;
    r.tag = tag;
    r.published = published.isValid() ? published.toUTC() : QDateTime::currentDateTimeUtc();
    r.prerelease = prerelease;
    r.files = assets;
    QByteArray sums;
    for (auto it = assets.begin(); it != assets.end(); ++it)
        sums += QCryptographicHash::hash(it.value(), QCryptographicHash::Sha256).toHex() + "  ./" + it.key().toUtf8() + "\n";
    r.files.insert(QStringLiteral("SHA256SUMS"), sums);
    releases_.prepend(r);
    writeList();
}

QString FixtureReleaseServer::downloadUrl(const QString& tag, const QString& name) const {
    return QStringLiteral("http://127.0.0.1:%1/download/%2/%3").arg(port_).arg(tag, name);
}

void FixtureReleaseServer::writeList() {
    QJsonArray list;
    for (const Release& r : releases_) {
        QJsonArray assets;
        for (auto it = r.files.begin(); it != r.files.end(); ++it) {
            QJsonObject a;
            a["name"] = it.key();
            a["size"] = double(it.value().size());
            a["browser_download_url"] = downloadUrl(r.tag, it.key());
            a["url"] = a["browser_download_url"];
            assets << a;
        }
        QJsonObject rel;
        rel["tag_name"] = r.tag;
        rel["draft"] = false;
        rel["prerelease"] = r.prerelease;
        rel["published_at"] = r.published.toString(Qt::ISODate);
        rel["html_url"] = QStringLiteral("http://127.0.0.1:%1/releases/tag/%2").arg(port_).arg(r.tag);
        rel["assets"] = assets;
        list << rel;
    }
    releasesJson_ = QJsonDocument(list).toJson(QJsonDocument::Compact);
}

void FixtureReleaseServer::corrupt(const QString& name) {
    for (Release& r : releases_) {
        if (!r.files.contains(name) || r.files[name].isEmpty()) continue;
        QByteArray& b = r.files[name];
        b[b.size() / 2] = char(b[b.size() / 2] ^ 0x5A);
        return;
    }
}

void FixtureReleaseServer::onConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        auto* buffer = new QByteArray();
        connect(s, &QTcpSocket::readyRead, this, [this, s, buffer] {
            *buffer += s->readAll();
            if (!buffer->contains("\r\n\r\n")) return;
            const QByteArray request = *buffer;
            buffer->clear();
            respond(s, request);
        });
        connect(s, &QTcpSocket::disconnected, s, [s, buffer] { delete buffer; s->deleteLater(); });
    }
}

void FixtureReleaseServer::respond(QTcpSocket* socket, const QByteArray& request) {
    ++requests_;
    const QList<QByteArray> words = request.left(request.indexOf("\r\n")).split(' ');
    const QByteArray path = words.size() > 1 ? words[1] : QByteArray();
    if (path == "/releases" || path.startsWith("/releases?")) { send(socket, 200, "application/json", releasesJson_); return; }
    if (path.startsWith("/download/")) {
        // /download/<tag>/<name>
        const QString rest = QString::fromUtf8(QByteArray::fromPercentEncoding(path.mid(10)));
        const int slash = rest.indexOf('/');
        const QString tag = rest.left(slash), name = rest.mid(slash + 1);
        for (const Release& r : releases_)
            if (slash > 0 && r.tag == tag && r.files.contains(name)) { send(socket, 200, "application/octet-stream", r.files.value(name)); return; }
    }
    send(socket, 404, "text/plain", "not found");
}

void FixtureReleaseServer::send(QTcpSocket* socket, int status, const QByteArray& type, const QByteArray& body) {
    QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + (status == 200 ? " OK" : " Not Found") + "\r\nContent-Type: " + type +
                      "\r\nContent-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
    socket->write(head);
    if (rate_ <= 0) {
        socket->write(body);
        served_ += body.size();
        socket->disconnectFromHost();
        return;
    }
    // a rate-limited body: one slice every 100 ms
    const qint64 slice = qMax<qint64>(1, rate_ / 10);
    auto* timer = new QTimer(socket);
    auto offset = std::make_shared<qint64>(0);
    QPointer<QTcpSocket> guard(socket);
    connect(timer, &QTimer::timeout, this, [this, guard, timer, offset, body, slice] {
        if (!guard) return;
        const qint64 n = qMin<qint64>(slice, body.size() - *offset);
        guard->write(body.mid(*offset, n));
        served_ += n;
        *offset += n;
        if (*offset >= body.size()) { timer->stop(); guard->disconnectFromHost(); }
    });
    timer->start(100);
}

}  // namespace ps2r::launcher
