// app/launcher/title_manifest.cpp -- reading title.json.
#include "title_manifest.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
namespace ps2r::launcher {

namespace {
QStringList stringList(const QJsonValue& v) {
    QStringList out;
    for (const QJsonValue& e : v.toArray()) out << e.toString();
    return out;
}
}  // namespace

bool TitleManifest::load(const QString& path, QString* error) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (error) *error = QStringLiteral("cannot read %1").arg(path);
        return false;
    }
    QJsonParseError perr;
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &perr);
    if (doc.isNull() || !doc.isObject()) {
        if (error) *error = QStringLiteral("%1 is not a JSON object: %2").arg(path, perr.errorString());
        return false;
    }
    path_ = path;
    return loadJson(doc.object(), error, QFileInfo(path).absolutePath());
}

bool TitleManifest::loadJson(const QJsonObject& root, QString* error, const QString& baseDir) {
    valid_ = false;
    id_ = root.value("id").toString();
    name_ = root.value("name").toString();
    product_ = root.value("product").toString(name_);
    description_ = root.value("description").toString();
    appDataFolder_ = root.value("app_data_folder").toString(product_);
    binaryName_ = root.value("binary_name").toString();
    if (id_.isEmpty() || name_.isEmpty()) {
        if (error) *error = QStringLiteral("the manifest needs an id and a name");
        return false;
    }

    const QJsonObject disc = root.value("disc").toObject();
    discManifests_.clear();
    for (const QString& m : stringList(disc.value("manifests"))) {
        discManifests_ << (QFileInfo(m).isAbsolute() || baseDir.isEmpty() ? m : QDir(baseDir).absoluteFilePath(m));
    }
    plays_ = stringList(disc.value("plays"));
    playsNote_ = disc.value("plays_note").toString();
    discExtensions_ = stringList(disc.value("extensions"));
    if (discExtensions_.isEmpty()) discExtensions_ << ".iso";

    const QJsonObject release = root.value("release").toObject();
    releasesUrl_ = release.value("releases_url").toString();
    pageUrl_ = release.value("page_url").toString();
    zipAsset_ = release.value("zip_asset").toString("{name}-{tag}-{target}.zip").replace("{name}", binaryName_);
    vitaAsset_ = release.value("vita_asset").toString("{name}-{tag}-vita.vpk").replace("{name}", binaryName_);
    sumsAsset_ = release.value("sums_asset").toString("SHA256SUMS");
    installBytes_ = qint64(release.value("install_bytes").toDouble(50.0 * 1024 * 1024));
    launcherAssets_.clear();
    const QJsonObject launcherAssets = release.value("launcher_assets").toObject();
    for (auto it = launcherAssets.begin(); it != launcherAssets.end(); ++it)
        if (!it.value().toString().isEmpty()) launcherAssets_.insert(it.key(), it.value().toString().replace("{name}", binaryName_));
    if (releasesUrl_.isEmpty()) {
        if (error) *error = QStringLiteral("the manifest needs release.releases_url (where the finished game is published)");
        return false;
    }

    const QJsonObject targets = root.value("targets").toObject();
    computers_.clear();
    for (const QJsonValue& v : targets.value("computers").toArray()) {
        const QJsonObject o = v.toObject();
        ComputerTarget t;
        t.id = o.value("id").toString();
        t.label = o.value("label").toString(t.id);
        t.shortLabel = o.value("short").toString(t.label);
        t.exe = o.value("exe").toString(binaryName_);
        if (!t.id.isEmpty()) computers_ << t;
    }
    const QJsonObject vita = targets.value("vita").toObject();
    vitaLabel_ = vita.value("label").toString();
    vitaFile_ = vita.value("file").toString();
    vitaNote_ = stringList(vita.value("note"));
    // the PS Vita folder: <game folder>/<folder>/ holding the package and <data_dir>/ as the console's card has it
    const QJsonObject vitaFolder = vita.value("folder").toObject();
    vitaFolderName_ = vitaFolder.value("name").toString("vita");
    vitaDataDir_ = vitaFolder.value("data_dir").toString();
    vitaReadme_ = vitaFolder.value("readme").toString("README-vita.txt");
    vitaDefaultDisc_ = vitaFolder.value("default_disc").toString("game.iso");
    vitaDroppedKeys_ = stringList(vitaFolder.value("settings_left_out"));
    vitaDiscsNote_ = stringList(vita.value("discs_note"));
    vitaModsNote_ = vita.value("mods_note").toString();
    vitaAbout_ = stringList(vita.value("about"));

    const QJsonObject run = root.value("run").toObject();
    runIsoFlag_ = run.value("iso_flag").toString("--iso");
    runConfigFlag_ = run.value("config_flag").toString("--config");
    runArgs_ = stringList(run.value("args"));
    titleStateLine_ = run.value("title_state_line").toString();
    titleStateName_ = run.value("title_state_name").toString();

    const QJsonObject folder = root.value("game_folder").toObject();
    gameFolderDefault_ = folder.value("default").toString("~/Games/" + name_);
    settingsFileName_ = folder.value("settings").toString("ps2r.ini");
    memcardsName_ = folder.value("memcards").toString("memcards");
    modsName_ = folder.value("mods").toString("mods");
    discFileName_ = folder.value("disc_file").toString();
    protectedNames_ = stringList(folder.value("protected"));
    for (const QString& n : {settingsFileName_, memcardsName_, modsName_, discFileName_})
        if (!n.isEmpty() && !protectedNames_.contains(n)) protectedNames_ << n;
    settingsHeader_ = folder.value("settings_header").toString();

    const QJsonObject settings = root.value("settings").toObject();
    settingsNote_ = settings.value("note").toString();
    settingsGroups_.clear();
    for (const QJsonValue& gv : settings.value("groups").toArray()) {
        const QJsonObject g = gv.toObject();
        SettingsGroupSpec group;
        group.title = g.value("title").toString();
        group.target = g.value("target").toString();
        for (const QJsonValue& rv : g.value("rows").toArray()) {
            const QJsonObject r = rv.toObject();
            SettingsRowSpec row;
            row.key = r.value("key").toString();
            row.label = r.value("label").toString(row.key);
            row.kind = r.value("kind").toString("choice");
            row.defaultValue = r.value("default").toString();
            row.help = r.value("help").toString();
            row.writeDefault = r.value("write_default").toBool(false);
            row.choicesFrom = r.value("choices_from").toString();
            for (const QJsonValue& cv : r.value("choices").toArray()) {
                const QJsonObject c = cv.toObject();
                row.choices << SettingsChoice{c.value("value").toString(), c.value("label").toString(c.value("value").toString())};
            }
            if (row.kind == "switch" && row.choices.isEmpty()) row.choices << SettingsChoice{"off", "Off"} << SettingsChoice{"on", "On"};
            // a derived row (choices_from) offers its default until the disc says more
            if (!row.choicesFrom.isEmpty() && row.choices.isEmpty() && !row.defaultValue.isEmpty()) {
                QString label = row.defaultValue;
                label[0] = label[0].toUpper();
                row.choices << SettingsChoice{row.defaultValue, label};
            }
            if (!row.key.isEmpty()) group.rows << row;
        }
        settingsGroups_ << group;
    }

    builtinMods_.clear();
    for (const QJsonValue& v : root.value("mods").toObject().value("builtin").toArray()) {
        const QJsonObject o = v.toObject();
        BuiltinMod m{o.value("id").toString(), o.value("name").toString(), o.value("author").toString(), o.value("help").toString(),
                     o.value("switch").toBool(true)};
        if (!m.id.isEmpty()) builtinMods_ << m;
    }

    const QJsonObject scan = root.value("scan").toObject();
    scanFolders_ = stringList(scan.value("folders"));
    scanRemovable_ = scan.value("removable").toBool(true);
    scanMaxDepth_ = scan.value("max_depth").toInt(2);
    scanMinBytes_ = qint64(scan.value("min_bytes").toDouble(32.0 * 1024 * 1024));
    licences_.clear();
    for (const QJsonValue& v : root.value("licences").toArray()) {
        const QJsonObject o = v.toObject();
        licences_ << LicenceLink{o.value("label").toString(), o.value("path").toString()};
    }
    valid_ = true;
    return true;
}

const ComputerTarget* TitleManifest::computer(const QString& id) const {
    for (const ComputerTarget& t : computers_) if (t.id == id) return &t;
    return nullptr;
}

QString TitleManifest::targetLabel(const QString& target) const {
    if (target == QLatin1String("vita")) return vitaLabel_;
    const ComputerTarget* c = computer(target);
    return c ? c->label : target;
}

QVector<SettingsRowSpec> TitleManifest::allSettingsRows() const {
    QVector<SettingsRowSpec> rows;
    for (const SettingsGroupSpec& g : settingsGroups_) rows += g.rows;
    return rows;
}

}  // namespace ps2r::launcher
