// app/launcher/mod_list.cpp -- reading the mods folder.
#include "mod_list.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <algorithm>
namespace ps2r::launcher {

namespace {
constexpr qint64 kManifestLimit = 1 << 20;   // the game reads a mod.json of at most 1 MB

QString firstSentence(const QString& text) {
    const QString t = text.simplified();
    const int end = t.indexOf(QRegularExpression(QStringLiteral("[.!?](\\s|$)")));
    return end < 0 ? t : t.left(end + 1);
}
}  // namespace

bool ModList::validId(const QString& id) {
    static const QRegularExpression re(QStringLiteral("^[a-z0-9_]{1,64}$"));
    return re.match(id).hasMatch();
}

ModEntry ModList::readOne(const QString& dir) {
    ModEntry e;
    e.dir = QDir::cleanPath(dir);
    e.id = QFileInfo(e.dir).fileName();
    e.name = e.id;
    const QString unreadable = QStringLiteral("This mod's description cannot be read.");
    QFile f(e.dir + "/mod.json");
    if (f.size() > kManifestLimit || !f.open(QIODevice::ReadOnly)) {
        e.problem = unreadable;
        return e;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull() || !doc.isObject()) {
        e.problem = unreadable;
        return e;
    }
    const QJsonObject m = doc.object();
    e.schema = m.value("schema").toInt(1);
    e.name = m.value("name").toString(e.id);
    if (e.name.trimmed().isEmpty()) e.name = e.id;
    e.author = m.value("author").toString();
    e.version = m.value("version").isDouble() ? QString::number(m.value("version").toDouble()) : m.value("version").toString();
    e.description = m.value("description").toString();
    e.kind = m.value("kind").toString(e.schema == 1 ? QStringLiteral("builtin") : QString());
    const QJsonObject source = m.value("source").toObject();
    e.sourceKind = source.value("kind").toString();
    const QJsonObject base = m.value("base").toObject();
    e.region = base.value("region").toString();
    e.executable = base.value("executable").toString();
    for (const QJsonValue& v : m.value("files").toArray()) {
        const QJsonObject row = v.toObject();
        const QString path = row.value("path").toString();
        if (!path.isEmpty()) e.files << path;
        // the game checks a recipe's base when it records one (mods_disc.cpp: form "ranges" and base_bytes)
        if (row.value("form").toString() == QLatin1String("ranges") && row.value("base_bytes").toDouble() > 0) e.baseChecked = true;
    }
    const QJsonObject exe = m.value("executable").toObject();
    const QJsonObject data = exe.value("data_changes").toObject();
    for (auto it = data.begin(); it != data.end(); ++it)
        if (it.value().isDouble()) e.dataChanges << qMakePair(it.key(), qint64(qMax(0.0, it.value().toDouble())));
    for (const QJsonValue& v : exe.value("code_changes").toArray())
        if (v.isString() && !v.toString().isEmpty()) e.codeChanges << v.toString();
    const QJsonObject language = m.value("language").toObject();
    e.languageId = language.value("id").toString();
    e.languageLabel = language.value("label").toString();
    e.languageBank = language.value("text_bank").toString();
    for (const QJsonValue& v : m.value("notes").toArray())
        if (v.isString() && !v.toString().trimmed().isEmpty()) e.notes << v.toString().trimmed();
    for (const QJsonValue& v : m.value("import").toObject().value("game_code_settings").toArray())
        if (v.isString() && !v.toString().trimmed().isEmpty()) e.gameCodeSettings << v.toString().trimmed();
    // the game's own checks (platform/nativeee/host/mods.cpp ScanFolder), in its order, as sentences
    const QString id = m.value("id").toString();
    if (!validId(e.id))
        e.problem = QStringLiteral("The game reads a mod only from a folder named in lower-case letters, digits and _.");
    else if (e.schema != 1 && e.schema != 2)
        e.problem = QStringLiteral("This mod is for a newer version of the game.");
    else if (id != e.id)
        e.problem = QStringLiteral("This mod's description names another folder (%1).").arg(id.isEmpty() ? QStringLiteral("none") : id);
    else if (e.kind == QLatin1String("builtin"))
        e.problem = QStringLiteral("This describes a mod that comes with the game; the copy here does nothing.");
    else if (e.kind != QLatin1String("disc") && e.kind != QLatin1String("language"))
        e.problem = QStringLiteral("The game does not read this kind of mod (%1).").arg(e.kind.isEmpty() ? QStringLiteral("none given") : e.kind);
    e.readable = e.problem.isEmpty();
    return e;
}

QVector<ModEntry> ModList::read(const QString& modsDir) {
    QVector<ModEntry> out;
    if (modsDir.isEmpty()) return out;
    for (const QFileInfo& fi : QDir(modsDir).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot, QDir::Name)) {
        const QString name = fi.fileName();
        if (name.startsWith('.')) continue;   // hidden: the import's .import-tmp, the file managers' folders
        if (!QFileInfo::exists(fi.absoluteFilePath() + "/mod.json")) continue;
        out << readOne(fi.absoluteFilePath());
    }
    return out;
}

QVector<ModEntry> ModList::mods(const QString& modsDir) {
    QVector<ModEntry> out;
    for (const ModEntry& e : read(modsDir))
        if (!isLanguagePack(e)) out << e;
    return out;
}

QVector<ModEntry> ModList::languagePacks(const QString& modsDir) {
    QVector<ModEntry> out;
    for (const ModEntry& e : read(modsDir))
        if (isLanguagePack(e) && e.readable) out << e;
    return out;
}

QStringList ModList::parseOrder(const QString& order) {
    QStringList out;
    for (const QString& id : order.split(QRegularExpression(QStringLiteral("[,;\\s]+")), Qt::SkipEmptyParts))
        if (!out.contains(id)) out << id;
    return out;
}

QString ModList::orderValue(const QStringList& ids) { return ids.join(','); }

QVector<ModEntry> ModList::ordered(const QVector<ModEntry>& entries, const QString& order) {
    QVector<ModEntry> out, rest, broken;
    const QStringList named = parseOrder(order);
    for (const QString& id : named)
        for (const ModEntry& e : entries)
            if (e.readable && e.id == id) out << e;
    for (const ModEntry& e : entries) {
        if (!e.readable) broken << e;
        else if (!named.contains(e.id)) rest << e;
    }
    std::sort(rest.begin(), rest.end(), [](const ModEntry& a, const ModEntry& b) { return a.id < b.id; });
    std::sort(broken.begin(), broken.end(), [](const ModEntry& a, const ModEntry& b) { return a.id < b.id; });
    return out + rest + broken;
}

QString ModList::regionLabel(const QString& region) {
    const QString r = region.trimmed();
    if (r.isEmpty()) return QString();
    if (r.compare(QLatin1String("any"), Qt::CaseInsensitive) == 0) return QStringLiteral("Any region");
    return r.toUpper();
}

QString ModList::byline(const QString& author, const QString& version) {
    QStringList parts;
    if (!author.trimmed().isEmpty()) parts << QStringLiteral("by %1").arg(author.trimmed());
    if (!version.trimmed().isEmpty()) parts << version.trimmed();
    return parts.join(QStringLiteral(" · "));
}

QString ModList::regionDisc(const QString& region) {
    const QString r = region.trimmed().toUpper();
    if (r == QLatin1String("PAL")) return QStringLiteral("the European disc");
    if (r == QLatin1String("NTSC-U")) return QStringLiteral("the North American disc");
    if (r == QLatin1String("NTSC-J")) return QStringLiteral("the Japanese disc");
    return QString();
}

QString ModList::summary(const ModEntry& e, const QString& discRegion) {
    QStringList parts;
    if (e.files.size() == 1) parts << QStringLiteral("Changes %1").arg(e.files.first());
    else if (e.files.size() > 1) parts << QStringLiteral("Changes %1 files").arg(e.files.size());
    qint64 data = 0;
    for (const auto& d : e.dataChanges) data += d.second;
    if (data > 0) parts << (parts.isEmpty() ? QStringLiteral("Data changes in the executable") : QStringLiteral("data changes in the executable"));
    // a mod made over another region's disc than the one the game starts with: the summary names the disc it was made for
    //; when the game also leaves it out there, the warning lines say so first (leftOutLine)
    const QString made = regionDisc(e.region);
    const bool other = !made.isEmpty() && !regionDisc(discRegion).isEmpty() && made != regionDisc(discRegion);
    if (parts.isEmpty()) {
        const QString first = firstSentence(e.description);
        if (!other) return first;
        return first.isEmpty() ? QStringLiteral("Made for %1.").arg(made) : QStringLiteral("%1 Made for %2.").arg(first, made);
    }
    return parts.join(QStringLiteral("; ")) + (other ? QStringLiteral(", made for %1.").arg(made) : QStringLiteral("."));
}

QString ModList::leftOutLine(const ModEntry& e, const QString& discRegion) {
    const QString made = regionDisc(e.region), here = regionDisc(discRegion);
    if (!e.baseChecked || made.isEmpty() || here.isEmpty() || made == here) return QString();
    return QStringLiteral("Left out on this disc: made for %1.").arg(made);
}

QString ModList::warningLines(const ModEntry& e, const QString& discRegion) {
    QStringList lines;
    const QString left = leftOutLine(e, discRegion);
    if (!left.isEmpty()) lines << left;
    const QString code = codeLine(e);
    if (!code.isEmpty()) lines << code;
    if (!e.gameCodeSettings.isEmpty())
        lines << QStringLiteral("It asks for Crate Mod Loader settings the game cannot apply: %1.").arg(e.gameCodeSettings.join(QStringLiteral(", ")));
    return lines.join('\n');
}

QString ModList::notesText(const ModEntry& e) { return e.notes.join('\n'); }

QString ModList::codeLine(const ModEntry& e) {
    if (e.codeChanges.isEmpty()) return QString();
    QStringList shown = e.codeChanges.mid(0, 3);
    QString line = QStringLiteral("Code changes not supported: %1").arg(shown.join(QStringLiteral(", ")));
    if (e.codeChanges.size() > 3) line += QStringLiteral(" and %1 more").arg(e.codeChanges.size() - 3);
    return line + ".";
}

}  // namespace ps2r::launcher
