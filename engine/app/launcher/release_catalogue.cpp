// app/launcher/release_catalogue.cpp -- the releases as data.
#include "release_catalogue.h"
#include <QCryptographicHash>
#include <QFile>
#include <QJsonObject>
#include <QSysInfo>
namespace ps2r::launcher {

QString ReleaseCatalogue::hostTarget() {
    const QString arch = QSysInfo::currentCpuArchitecture();
#ifdef Q_OS_MACOS
    return arch == "arm64" ? QStringLiteral("macos-arm64") : QStringLiteral("macos-x86_64");
#elif defined(Q_OS_WIN)
    return QStringLiteral("windows-x64");
#else
    return arch == "arm64" ? QStringLiteral("linux-aarch64") : QStringLiteral("linux-x86_64");
#endif
}

QString ReleaseCatalogue::assetName(const QString& pattern, const QString& tag, const QString& target) {
    QString name = pattern;
    name.replace("{tag}", tag);
    name.replace("{version}", tag);
    name.replace("{target}", target);
    return name;
}

bool ReleaseCatalogue::pick(const QJsonArray& releases, const QString& pattern, const QString& target, const QString& sumsName,
                            bool apiUrls, ReleaseChoice* out) {
    for (const QJsonValue& rv : releases) {
        const QJsonObject rel = rv.toObject();
        if (rel.value("draft").toBool() || rel.value("prerelease").toBool()) continue;
        const QString tag = rel.value("tag_name").toString();
        if (tag.isEmpty()) continue;
        const QString want = assetName(pattern, tag, target);
        ReleaseAsset asset, sums;
        for (const QJsonValue& v : rel.value("assets").toArray()) {
            const QJsonObject a = v.toObject();
            ReleaseAsset r;
            r.name = a.value("name").toString();
            r.url = QUrl(apiUrls ? a.value("url").toString() : a.value("browser_download_url").toString());
            r.bytes = qint64(a.value("size").toDouble());
            if (r.name == want) asset = r;
            if (r.name == sumsName) sums = r;
        }
        if (!asset.name.isEmpty() && asset.url.isValid() && !sums.name.isEmpty() && sums.url.isValid()) {
            out->tag = tag;
            out->asset = asset;
            out->sums = sums;
            return true;
        }
    }
    return false;
}

QString ReleaseCatalogue::shaFor(const QString& sumsText, const QString& assetName) {
    for (const QString& line : sumsText.split('\n')) {
        const QString l = line.simplified();
        const int space = l.indexOf(' ');
        if (space != 64) continue;
        QString file = l.mid(space + 1);
        if (file.startsWith('*')) file.remove(0, 1);
        if (file.startsWith(QLatin1String("./"))) file.remove(0, 2);
        if (file == assetName) return l.left(64).toLower();
    }
    return QString();
}

QString ReleaseCatalogue::sha256Of(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) return QString();
    QCryptographicHash h(QCryptographicHash::Sha256);
    if (!h.addData(&f)) return QString();
    return QString::fromLatin1(h.result().toHex());
}

QString ReleaseCatalogue::bytesText(qint64 bytes) {
    if (bytes < 0) return QStringLiteral("?");
    if (bytes < 1024 * 1024) return QStringLiteral("%1 KB").arg((bytes + 512) / 1024);
    if (bytes < 1024ll * 1024 * 1024) {
        const double mb = double(bytes) / (1024.0 * 1024);
        return mb >= 100 || qFuzzyCompare(mb, double(qRound(mb))) ? QStringLiteral("%1 MB").arg(qRound(mb)) : QStringLiteral("%1 MB").arg(mb, 0, 'f', 1);
    }
    return QStringLiteral("%1 GB").arg(double(bytes) / (1024.0 * 1024 * 1024), 0, 'f', 1);
}

QString ReleaseCatalogue::rateText(double bytesPerSecond) {
    if (bytesPerSecond < 1024 * 1024) return QStringLiteral("%1 KB/s").arg(qRound(bytesPerSecond / 1024));
    return QStringLiteral("%1 MB/s").arg(bytesPerSecond / (1024.0 * 1024), 0, 'f', 1);
}

}  // namespace ps2r::launcher
