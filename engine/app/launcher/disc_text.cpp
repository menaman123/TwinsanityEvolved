// app/launcher/disc_text.cpp -- a disc's words.
#include "disc_text.h"
#include <QFileInfo>
#include "app_data.h"
#include "disc_scanner.h"
#include "release_catalogue.h"
namespace ps2r::launcher {

namespace {
bool checkingNow(const DiscFound& d) { return d.checking || d.verdict.isEmpty(); }
}  // namespace

QString DiscText::regionBadge(const DiscFound& d) {
    return d.region.isEmpty() || d.region == QLatin1String("unknown region") ? QStringLiteral("Unknown region") : d.region;
}

QString DiscText::identityBadge(const DiscFound& d) {
    QString what;
    if (checkingNow(d)) what = QStringLiteral("checking…");
    else if (d.verdict == QLatin1String("retail")) what = QStringLiteral("original disc");
    else if (d.verdict == QLatin1String("modified")) what = QStringLiteral("modified disc");
    else if (d.verdict == QLatin1String("retail-size")) what = QStringLiteral("not checked");
    else what = QStringLiteral("unrecognised version");
    return regionBadge(d) + QStringLiteral(" · ") + what;
}

Badge::Tone DiscText::identityTone(const DiscFound& d) {
    if (checkingNow(d)) return Badge::Neutral;
    if (d.verdict == QLatin1String("retail")) return Badge::Ok;
    if (d.verdict == QLatin1String("modified")) return Badge::Accent;
    if (d.verdict == QLatin1String("retail-size")) return Badge::Neutral;
    return Badge::Warn;
}

QString DiscText::stateBadge(const DiscFound&, bool playable) {
    return playable ? QString() : QStringLiteral("Not playable in this version");
}

Badge::Tone DiscText::stateTone(const DiscFound&, bool playable) { return playable ? Badge::Neutral : Badge::Warn; }

QString DiscText::verdictLine(const DiscFound& d) {
    const QString region = regionBadge(d) == QLatin1String("Unknown region") ? QString() : regionBadge(d) + QLatin1Char(' ');
    if (checkingNow(d)) return QStringLiteral("Checking whether this is the original disc…");
    if (d.verdict == QLatin1String("retail")) return QString();
    if (d.verdict == QLatin1String("retail-size")) return QStringLiteral("It has the size of the original disc; its contents were not checked.");
    if (d.verdict == QLatin1String("modified")) {
        QString line = QStringLiteral("A modified copy of the %1disc").arg(region);
        if (!d.differences.isEmpty()) {
            QStringList named = d.differences.mid(0, 3);
            line += QStringLiteral(". These files differ from the original: %1").arg(named.join(QStringLiteral(", ")));
            if (d.differences.size() > 3) line += QStringLiteral(" and %1 more").arg(d.differences.size() - 3);
            return line + QLatin1Char('.');
        }
        if (d.dumpBytes > 0 && d.imageBytes > d.dumpBytes)
            return line + QStringLiteral(": %1 larger than the original.").arg(ReleaseCatalogue::bytesText(d.imageBytes - d.dumpBytes));
        if (d.dumpBytes > 0 && d.imageBytes < d.dumpBytes)
            return line + QStringLiteral(": %1 smaller than the original.").arg(ReleaseCatalogue::bytesText(d.dumpBytes - d.imageBytes));
        return line + QStringLiteral(": the size of the original, with other contents.");
    }
    return QStringLiteral("It may not play as expected.");
}

QString DiscText::fileName(const DiscFound& d) { return QFileInfo(d.path).fileName(); }

QString DiscText::detailsLine(const DiscFound& d) {
    QStringList parts;
    if (!d.serial.isEmpty()) parts << d.serial;
    parts << ReleaseCatalogue::bytesText(d.sizeBytes);
    parts << AppData::shortPath(QFileInfo(d.path).absolutePath());
    return parts.join(QStringLiteral(" · "));
}

QString DiscText::stateLine(const DiscFound& d, bool playable, const QString& notPlayable) {
    if (!playable) return notPlayable;
    return verdictLine(d);
}

}  // namespace ps2r::launcher
