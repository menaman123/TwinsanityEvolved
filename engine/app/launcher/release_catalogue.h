// app/launcher/release_catalogue.h -- the releases as data: which published release carries a target's game, the
// asset names (twinsanityevolved-<tag>-<target>.zip, twinsanityevolved-<tag>-vita.vpk), the SHA256SUMS lines, this
// computer's target, and the byte / rate texts the progress line shows. No network here (ReleaseInstaller fetches).
#pragma once
#include <QJsonArray>
#include <QString>
#include <QUrl>
namespace ps2r::launcher {

struct ReleaseAsset {
    QString name;
    QUrl url;            // the public download URL (or the API's asset URL when a token is used)
    qint64 bytes = 0;
};

struct ReleaseChoice {
    QString tag;         // v0.3.0
    ReleaseAsset asset;  // the target's zip or vpk
    ReleaseAsset sums;   // SHA256SUMS
};

class ReleaseCatalogue {
public:
    // macos-arm64 | macos-x86_64 | windows-x64 | linux-x86_64 | linux-aarch64
    static QString hostTarget();
    // The asset's name: the pattern's {tag} and {target} replaced ("vita" uses the vpk pattern).
    static QString assetName(const QString& pattern, const QString& tag, const QString& target);
    // The newest release in `releases` (the API's order: newest first) that is neither a draft nor a prerelease and
    // carries both the target's asset and `sumsName`. `apiUrls` picks the assets' API URLs (a token is set) over the
    // public download URLs. False when none has them.
    static bool pick(const QJsonArray& releases, const QString& pattern, const QString& target, const QString& sumsName,
                     bool apiUrls, ReleaseChoice* out);
    // The sha256 SHA256SUMS gives `assetName` ("<hex>  ./name", "<hex> *name" and "<hex>  name" all read); "" when absent.
    static QString shaFor(const QString& sumsText, const QString& assetName);
    static QString sha256Of(const QString& path);   // lower-case hex, "" when unreadable
    static QString bytesText(qint64 bytes);          // 12.4 MB
    static QString rateText(double bytesPerSecond);  // 3.2 MB/s
};

}  // namespace ps2r::launcher
