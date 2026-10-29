// tests/app/launcher/fixture_release_server.h -- a releases server for the launcher's tests: 127.0.0.1 only, HTTP/1.1,
// GET /releases (a GitHub-API-shaped JSON array, newest first: each release's tag, published_at, html_url and assets)
// and GET /download/<tag>/<asset>, with an optional rate limit (to hold a download in progress for a screenshot) and an
// offline switch (connections refused). publish() serves ONE release; addRelease() adds another on top (two launcher
// versions for the self-update). Counts the bytes it served. Offline-safe: nothing leaves the machine.
#pragma once
#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QObject>
#include <QString>
#include <QVector>
class QTcpServer;
class QTcpSocket;
namespace ps2r::launcher {

class FixtureReleaseServer : public QObject {
    Q_OBJECT
public:
    explicit FixtureReleaseServer(QObject* parent = nullptr);
    bool start();   // listens on a free port of 127.0.0.1
    void stop();    // offline: the port is closed, every request is refused
    quint16 port() const { return port_; }
    QString releasesUrl() const;   // http://127.0.0.1:<port>/releases
    // One published release `tag` carrying `assets` (name -> bytes); SHA256SUMS over them is added.
    void publish(const QString& tag, const QHash<QString, QByteArray>& assets);
    // Another release on top of the list (the newest), published at `published` (UTC; invalid = now): its own assets and
    // SHA256SUMS. A draft or a prerelease when asked.
    void addRelease(const QString& tag, const QHash<QString, QByteArray>& assets, const QDateTime& published = QDateTime(),
                    bool prerelease = false);
    void clear();   // no release at all
    // Serves `name` (of the newest release that has it) with one byte changed, SHA256SUMS still naming the published
    // bytes (a damaged download).
    void corrupt(const QString& name);
    QString downloadUrl(const QString& tag, const QString& name) const;
    void setRateLimit(qint64 bytesPerSecond) { rate_ = bytesPerSecond; }   // 0 = as fast as possible
    qint64 bytesServed() const { return served_; }
    int requests() const { return requests_; }

private:
    void onConnection();
    void respond(QTcpSocket* socket, const QByteArray& request);
    void send(QTcpSocket* socket, int status, const QByteArray& type, const QByteArray& body);
    void writeList();
    struct Release {
        QString tag;
        QDateTime published;
        bool prerelease = false;
        QHash<QString, QByteArray> files;   // with its SHA256SUMS
    };
    QTcpServer* server_;
    quint16 port_ = 0;
    QByteArray releasesJson_;
    QVector<Release> releases_;   // newest first
    qint64 rate_ = 0, served_ = 0;
    int requests_ = 0;
};

}  // namespace ps2r::launcher
