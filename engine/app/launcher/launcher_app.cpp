// app/launcher/launcher_app.cpp -- the model wired together.
#include "launcher_app.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcessEnvironment>
#include <QSaveFile>
#include <QTimer>
#include "app/host/disc_identity.h"
#include "app/host/disc_languages.h"
#include "app/host/disc_regions.h"
#include "app_data.h"
#include "game_folder.h"
#include "game_library.h"
#include "game_process.h"
#include "ini_settings.h"
#include "mod_list.h"
#include "release_catalogue.h"
#include "launcher_version.h"
#include "release_installer.h"
#include "self_update.h"
#include "title_manifest.h"
#include "vita_folder.h"
namespace ps2r::launcher {

LauncherApp::LauncherApp(QObject* parent) : QObject(parent) {}

LauncherApp::~LauncherApp() {
    if (installer_) installer_->disconnect(this);
    if (game_) game_->disconnect(this);
    delete scanner_;   // waits for a worker still identifying a disc
    scanner_ = nullptr;
    delete manifest_;
    delete appData_;
}

QStringList LauncherApp::manifestSearchPaths() {
    QStringList dirs;
    const QString env = QProcessEnvironment::systemEnvironment().value("PS2R_LAUNCHER_MANIFEST");   // lint: not-ui
    if (!env.isEmpty()) dirs << env;
    const QString app = QCoreApplication::applicationDirPath();
    dirs << app + "/../Resources/titles" << app + "/titles" << app + "/../titles" << app + "/../../titles";
    return dirs;
}

bool LauncherApp::loadManifest(const QString& explicitPath, QString* error) {
    QStringList candidates;
    if (!explicitPath.isEmpty()) candidates << explicitPath;
    for (const QString& p : manifestSearchPaths()) {
        const QFileInfo fi(p);
        if (fi.isFile()) candidates << p;
        else if (fi.isDir()) for (const QString& f : QDir(p).entryList({"*.json"}, QDir::Files, QDir::Name)) candidates << p + "/" + f;
    }
    QString firstError;
    for (const QString& c : candidates) {
        auto* m = new TitleManifest();
        QString err;
        if (m->load(c, &err)) { manifest_ = m; break; }
        if (firstError.isEmpty()) firstError = err;
        delete m;
    }
    if (!manifest_) {
        if (error) *error = candidates.isEmpty() ? QStringLiteral("No title manifest was found beside the application (titles/*.json).") : firstError;
        return false;
    }
    // the disc tables the discs are identified with (the same files the game builds in): the known retail dumps
    // (discs/redump.json) and the file lists (discs/<id>.json), each read by its kind
    ps2r::app::ClearRetailManifests();
    ps2r::app::ClearKnownDumps();
    for (const QString& list : manifest_->discManifests()) {
        std::string err;
        if (!ps2r::app::LoadDiscTable(QFile::encodeName(list).toStdString(), &err))
            qWarning("disc table %s not read: %s", qPrintable(list), err.c_str());   // lint: not-ui
    }
    appData_ = new AppData(manifest_->appDataFolder());
    QString err;
    appData_->ensureFolders(&err);
    library_ = new GameLibrary(appData_, this);
    installer_ = new ReleaseInstaller(appData_, manifest_, this);
    game_ = new GameProcess(manifest_, this);
    selfUpdate_ = new SelfUpdate(appData_, manifest_, this);
    // the launcher restarts to update: never while the game it started is running (closing the window stops the game)
    selfUpdate_->setGameRunning([this] { return game_->running(); });
    // the first start after an update: the copy it replaced goes, the update is told once
    selfUpdate_->finishAfterRelaunch();
    scanner_ = new DiscScanner();
    scanner_->setCache(appData_->value("identities").toObject());
    connect(scanner_, &DiscScanner::refused, this, [this](const DiscFound& d) {
        appData_->setValue("identities", scanner_->cache());
        appData_->save();
        emit discRefused(d.path, d.refusal);
    });
    connect(scanner_, &DiscScanner::found, this, [this](const DiscFound& d) {
        if (d.fromSearch) return;   // a search's results are saved when it finishes
        appData_->setValue("identities", scanner_->cache());
        appData_->save();
    });
    connect(scanner_, &DiscScanner::finished, this, [this](int, int) {
        appData_->setValue("identities", scanner_->cache());
        appData_->save();
    });
    // the whole-image check finished: the verdict is kept, and a disc already on the list takes it
    connect(scanner_, &DiscScanner::checked, this, [this](const DiscFound& d) {
        appData_->setValue("identities", scanner_->cache());
        appData_->save();
        library_->updateIdentity(d);
    });
    // discs on the list identified by an older launcher (or whose check never finished) are checked again, once the
    // pages listen (the first turn of the event loop)
    QTimer::singleShot(0, this, [this] {
        for (const QString& path : library_->staleIdentities()) scanner_->identify(path);
    });
    connect(installer_, &ReleaseInstaller::finished, this, [this](bool ok) {
        if (ok) finishInstall();
        emit installFinished(ok);
        emit stateChanged();
    });
    connect(installer_, &ReleaseInstaller::latestFound, this, [this](const QString& target, const QString& tag) {
        const InstallRecord r = installFor(target);
        if (r.target.isEmpty() || r.tag == tag) { updates_.remove(target); return; }
        updates_.insert(target, tag);
        emit updateAvailable(target, tag);
        emit stateChanged();
    });
    connect(game_, &GameProcess::finished, this, [this](bool, qint64 seconds) {
        if (!playingIso_.isEmpty()) library_->recordPlayEnd(playingIso_, seconds);
        playingIso_.clear();
        emit stateChanged();
    });
    connect(library_, &GameLibrary::changed, this, &LauncherApp::stateChanged);
    return true;
}

QString LauncherApp::hostTarget() const { return ReleaseCatalogue::hostTarget(); }

QVector<InstallRecord> LauncherApp::installs() const {
    QVector<InstallRecord> out;
    for (const QJsonValue& v : appData_->value("installs").toArray()) {
        const QJsonObject o = v.toObject();
        InstallRecord r{o.value("target").toString(), o.value("folder").toString(), o.value("tag").toString(),
                        o.value("placed").toString(), QDateTime::fromString(o.value("at").toString(), Qt::ISODate)};
        if (!r.target.isEmpty() && !r.folder.isEmpty()) out << r;
    }
    return out;
}

void LauncherApp::saveInstalls(const QVector<InstallRecord>& installs) {
    QJsonArray arr;
    for (const InstallRecord& r : installs) {
        QJsonObject o;
        o["target"] = r.target;
        o["folder"] = r.folder;
        o["tag"] = r.tag;
        o["placed"] = r.placed;
        o["at"] = r.at.toString(Qt::ISODate);
        arr << o;
    }
    appData_->setValue("installs", arr);
    appData_->save();
}

InstallRecord LauncherApp::installFor(const QString& target) const {
    for (const InstallRecord& r : installs()) if (r.target == target) return r;
    return InstallRecord{};
}

InstallRecord LauncherApp::playableInstall() const {
    const InstallRecord r = installFor(hostTarget());
    return !r.placed.isEmpty() && QFileInfo(r.placed).isExecutable() ? r : InstallRecord{};
}

bool LauncherApp::setupDone() const { return !installs().isEmpty(); }

QString LauncherApp::gameFolder() const {
    const InstallRecord here = installFor(hostTarget());
    if (!here.folder.isEmpty()) return here.folder;
    // another computer's install; a PS Vita install is a copy made FROM a game folder, not one
    for (const InstallRecord& r : installs())
        if (r.target != QLatin1String("vita")) return r.folder;
    const QString chosen = appData_->value("chosen_folder").toString();
    if (!chosen.isEmpty()) return chosen;
    const InstallRecord vita = installFor(QStringLiteral("vita"));
    if (vita.folder.isEmpty()) return QString();
    const QString home = VitaFolder::homeOf(manifest_, vita.folder);
    return home.isEmpty() ? vita.folder : home;
}

void LauncherApp::setChosenFolder(const QString& folder) {
    appData_->setValue("chosen_folder", QDir::cleanPath(folder));
    appData_->save();
    emit stateChanged();
}

void LauncherApp::startInstall(const QString& target, const QString& folder) {
    if (installer_->running()) return;
    installer_->install(target, folder);
    emit stateChanged();
}

void LauncherApp::finishInstall() {
    const QString target = installer_->target();
    const QString folder = installer_->folder();
    const GameFolder gf(manifest_, folder);
    QString err;
    if (target == QLatin1String("vita")) {
        // the folder made ready to copy: the README, the console's data folder; the earlier versions' note goes
        QFile::remove(folder + "/" + manifest_->vitaLabel() + " - how to install.txt");
        QVector<InstallRecord> all = installs();
        for (int i = all.size() - 1; i >= 0; --i) if (all[i].target == target) all.remove(i);
        all.prepend(InstallRecord{target, folder, installer_->tag(), installer_->placedFile(), QDateTime::currentDateTime()});
        saveInstalls(all);   // recorded first: makeVitaFolder() finds the install's folder
        makeVitaFolder(&vitaReport_);
    } else {
        gf.prepare(settingDefaultsFor(gf.settingsFile()), &err);
        if (target == hostTarget()) {
            // the disc the game starts with on its own: the one played last, else the first card
            QString disc = gf.disc();
            if (disc.isEmpty() || !QFileInfo::exists(disc)) {
                for (const GameEntry& g : library_->games()) if (!g.missing) { disc = g.disc.path; break; }
            }
            if (!disc.isEmpty()) gf.setDisc(disc, &err);
        }
    }
    QVector<InstallRecord> all = installs();
    for (int i = all.size() - 1; i >= 0; --i)
        if (all[i].target == target || QDir::cleanPath(all[i].folder) == QDir::cleanPath(folder)) all.remove(i);
    all.prepend(InstallRecord{target, folder, installer_->tag(), installer_->placedFile(), QDateTime::currentDateTime()});
    saveInstalls(all);
    updates_.remove(target);
    if (target == hostTarget()) setChosenFolder(folder);
    emit installsChanged();
    emit settingsChanged(QString());
}

QString LauncherApp::vitaFolderFor(const QString& gameFolder) const {
    return VitaFolder::folderIn(manifest_, gameFolder.isEmpty() ? GameFolder::suggested(manifest_) : gameFolder);
}

QString LauncherApp::vitaFolder() const { return installFor(QStringLiteral("vita")).folder; }

QString LauncherApp::vitaConsoleDisc() const {
    const QString home = gameFolder();
    QString disc = home.isEmpty() ? QString() : GameFolder(manifest_, home).disc();
    if (disc.isEmpty() || !QFileInfo::exists(disc))
        for (const GameEntry& g : library_->games()) if (!g.missing) { disc = g.disc.path; break; }
    return VitaFolder::consoleDiscName(manifest_, disc);
}

bool LauncherApp::makeVitaFolder(VitaFolderReport* report) {
    VitaFolderReport r;
    const QString folder = vitaFolder();
    if (folder.isEmpty()) {
        r.error = QStringLiteral("There is no %1 version yet: get it from the Games page first.").arg(manifest_->vitaLabel());
        if (report) *report = r;
        return false;
    }
    // the disc to name when the game folder names none: the first disc on the Games page that is still there
    QString fallback;
    for (const GameEntry& g : library_->games()) if (!g.missing) { fallback = g.disc.path; break; }
    QString home = gameFolder();
    if (QDir::cleanPath(home) == QDir::cleanPath(folder)) home.clear();   // an earlier version's PS Vita folder alone: no settings to copy
    const bool ok = VitaFolder::make(manifest_, home, folder, fallback, &r);
    vitaReport_ = r;
    if (report) *report = r;
    emit stateChanged();
    return ok;
}

QString LauncherApp::updateFor(const QString& target) const { return updates_.value(target); }

void LauncherApp::checkForUpdates() {
    for (const InstallRecord& r : installs()) installer_->checkLatest(r.target);
}

void LauncherApp::checkLauncherUpdate() { selfUpdate_->check(); }

void LauncherApp::scan() {
    QStringList folders;
    for (const QString& f : manifest_->scanFolders()) folders << appData_->expand(f);
    folders.removeDuplicates();
    scanner_->start(folders, manifest_->scanRemovable(), manifest_->scanMaxDepth(), manifest_->discExtensions(), manifest_->scanMinBytes());
}

void LauncherApp::identify(const QString& path) { scanner_->identify(path); }

void LauncherApp::addDisc(const DiscFound& disc) {
    if (!disc.accepted) return;
    library_->add(disc);
}

void LauncherApp::setDiscsChosen(const QVector<DiscFound>& discs) {
    QStringList chosen;
    for (const DiscFound& d : discs) chosen << d.path;
    for (const QString& p : setupAdded_)
        if (!chosen.contains(p)) library_->remove(p, false);   // a changed choice, not a removal
    setupAdded_.clear();
    for (const DiscFound& d : discs) {
        if (!library_->game(d.path)) setupAdded_ << d.path;
        DiscFound explicitChoice = d;
        explicitChoice.fromSearch = false;   // the player chose it in step 1
        library_->add(explicitChoice);
    }
}

LauncherApp::Action LauncherApp::nextAction(const QString& isoPath) const {
    const GameEntry* g = library_->game(isoPath);
    if (!g || g->missing) return Missing;
    if (game_->running() && game_->isoPath() == isoPath) return Playing;
    if (!manifest_->playableExecutables().isEmpty() &&
        !manifest_->playableExecutables().contains(g->disc.executable, Qt::CaseInsensitive)) return NotPlayable;
    if (playableInstall().target.isEmpty()) return NotInstalled;
    return Play;
}

QString LauncherApp::notPlayableReason(const QString& isoPath) const {
    const GameEntry* g = library_->game(isoPath);
    if (!g) return QString();
    // another release's disc: the region table says what it needs (the game's own refusal, engine/app/host/disc_regions.h)
    const std::string region = ps2r::app::RegionNotPlayable(g->disc.serial.toStdString());
    if (!region.empty()) return QString::fromStdString(region);
    return manifest_->playsNote().isEmpty() ? QStringLiteral("This version of the game cannot play this disc.") : manifest_->playsNote();
}

bool LauncherApp::play(const QString& isoPath, QString* error) {
    if (game_->running()) { if (error) *error = QStringLiteral("The game is already running."); return false; }
    const InstallRecord r = playableInstall();
    if (r.target.isEmpty()) { if (error) *error = QStringLiteral("The game is not installed on this computer yet."); return false; }
    const GameFolder gf(manifest_, r.folder);
    QString err;
    gf.setDisc(isoPath, &err);   // the game's own shortcut then starts with the disc played last
    if (!game_->start(r.placed, isoPath, gf.settingsFile(), {}, appData_->logPath("play"), error)) return false;
    playingIso_ = isoPath;
    library_->recordPlayStart(isoPath);
    emit stateChanged();
    return true;
}

void LauncherApp::stopGame() { game_->stop(); }

QString LauncherApp::settingsFilePath() const {
    const QString folder = gameFolder();
    return folder.isEmpty() ? QString() : GameFolder(manifest_, folder).settingsFile();
}

QString LauncherApp::settingValue(const QString& key) const {
    const QString file = settingsFilePath();
    const QString v = file.isEmpty() ? QString() : IniSettings(file).get(key);
    if (!v.isEmpty()) return v;
    for (const SettingsRowSpec& r : manifest_->allSettingsRows()) if (r.key == key) return r.defaultValue;
    return QString();
}

bool LauncherApp::setSettingValue(const QString& key, const QString& value, QString* error) {
    const QString file = settingsFilePath();
    if (file.isEmpty()) { if (error) *error = QStringLiteral("the game is not set up yet"); return false; }
    if (!IniSettings(file).set(key, value, error)) return false;
    emit settingsChanged(key);
    return true;
}

bool LauncherApp::removeSettingValue(const QString& key, QString* error) {
    const QString file = settingsFilePath();
    if (file.isEmpty()) { if (error) *error = QStringLiteral("the game is not set up yet"); return false; }
    if (!IniSettings(file).remove(key, error)) return false;
    emit settingsChanged(key);
    return true;
}

QString LauncherApp::folderSetting(const QString& key) const {
    const QString folder = gameFolder();
    if (folder.isEmpty()) return QString();
    QString fallback = key == QLatin1String("paths.mods") ? manifest_->modsName() : manifest_->memcardsName();
    for (const SettingsRowSpec& r : manifest_->allSettingsRows()) if (r.key == key && !r.defaultValue.isEmpty()) fallback = r.defaultValue;
    return GameFolder(manifest_, folder).folderSetting(key, fallback);
}

bool LauncherApp::setFolderSetting(const QString& key, const QString& dir, QString* error) {
    const QString folder = gameFolder();
    if (folder.isEmpty()) { if (error) *error = QStringLiteral("the game is not set up yet"); return false; }
    if (!GameFolder(manifest_, folder).setFolderSetting(key, dir, error)) return false;
    emit settingsChanged(key);
    return true;
}

QString LauncherApp::discSetting() const {
    const QString folder = gameFolder();
    return folder.isEmpty() ? QString() : GameFolder(manifest_, folder).disc();
}

bool LauncherApp::setDiscSetting(const QString& iso, QString* error) {
    const QString folder = gameFolder();
    if (folder.isEmpty()) { if (error) *error = QStringLiteral("the game is not set up yet"); return false; }
    if (!GameFolder(manifest_, folder).setDisc(iso, error)) return false;
    emit settingsChanged(manifest_->discFileName());
    return true;
}

QString LauncherApp::modsFolder() const { return folderSetting(QStringLiteral("paths.mods")); }

QString LauncherApp::importBaseDisc() const {
    const QString disc = discSetting();
    if (!disc.isEmpty() && QFileInfo(disc).isFile()) return disc;
    for (const GameEntry& g : library_->games())
        if (!g.missing && QFileInfo(g.disc.path).isFile()) return g.disc.path;
    return QString();
}

QString LauncherApp::importBaseRegion() const {
    const QString disc = importBaseDisc();
    if (disc.isEmpty()) return QString();
    QString r;
    if (const GameEntry* g = library_->game(disc)) r = g->disc.region.trimmed();
    else r = DiscScanner::identifyNow(disc, false).region;   // the quick identity: the directories and the program
    return (r == QLatin1String("PAL") || r == QLatin1String("NTSC-U") || r == QLatin1String("NTSC-J")) ? r : QString();
}

QVector<QPair<QString, QString>> LauncherApp::settingDefaults() const {
    QVector<QPair<QString, QString>> out;
    for (const SettingsRowSpec& r : manifest_->allSettingsRows())
        if (r.writeDefault && r.kind != QLatin1String("disc")) out << qMakePair(r.key, r.defaultValue);
    return out;
}

QVector<QPair<QString, QString>> LauncherApp::settingDefaultsFor(const QString& settingsFile) const {
    QVector<QPair<QString, QString>> out = settingDefaults();
    // an older folder's game.language stays in charge of both languages until the player changes a language row: the two
    // keys are not added beside it (their defaults would win over the player's choice)
    if (!IniSettings(settingsFile).has(QStringLiteral("game.language"))) return out;
    for (int i = int(out.size()) - 1; i >= 0; --i)
        for (const SettingsRowSpec& r : manifest_->allSettingsRows())
            if (r.key == out[i].first && !r.choicesFrom.isEmpty()) { out.remove(i); break; }
    return out;
}

LauncherApp::LanguageChoices LauncherApp::languageChoices() const {
    const QString disc = importBaseDisc();
    const QString mods = modsFolder();
    const QVector<ModEntry> packs = mods.isEmpty() ? QVector<ModEntry>() : ModList::languagePacks(mods);
    // what the answer depends on: the disc (path, size, time) and each pack's bank (path, size, time)
    QString key = disc;
    if (!disc.isEmpty()) {
        const QFileInfo fi(disc);
        key += QStringLiteral("|%1|%2").arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    }
    for (const ModEntry& p : packs) {
        const QFileInfo fi(p.dir + "/" + p.languageBank);
        key += QStringLiteral("|%1|%2|%3").arg(fi.absoluteFilePath()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    }
    if (key == languagesKey_ && !languagesKey_.isNull()) return languages_;
    LanguageChoices c;
    ps2r::app::DiscLanguages dl;
    if (!disc.isEmpty() && ps2r::app::ReadDiscLanguages(QFile::encodeName(disc).toStdString(), &dl)) {
        c.disc = disc;
        for (const std::string& t : dl.text) c.text << QString::fromStdString(t);
        for (const std::string& t : dl.speech) c.speech << QString::fromStdString(t);
        // a language pack is offered when its bank was made from this disc's English (the game's own check)
        for (const ModEntry& p : packs) {
            QFile bank(p.dir + "/" + p.languageBank);
            if (p.languageBank.isEmpty() || bank.size() > (64 << 20) || !bank.open(QIODevice::ReadOnly)) continue;
            std::string code_key, agentlab_key;
            ps2r::app::BankKeys(bank.readAll().toStdString(), &code_key, &agentlab_key);
            const QString name = (p.languageId.isEmpty() ? p.id : p.languageId).toLower();
            if (ps2r::app::PackFits(dl, code_key, agentlab_key) && !c.text.contains(name)) c.text << name;
        }
    }
    if (c.text.isEmpty()) c.text << QStringLiteral("english");
    if (c.speech.isEmpty()) c.speech << QStringLiteral("english");
    languagesKey_ = key.isNull() ? QStringLiteral("") : key;
    languages_ = c;
    return c;
}

QString LauncherApp::languageValue(const QString& key) const {
    const QString file = settingsFilePath();
    const IniSettings ini(file);
    const LanguageChoices c = languageChoices();
    const bool text = key == QLatin1String("game.text_language");
    QString v = file.isEmpty() ? QString() : ini.get(key).trimmed().toLower();
    if (v.isEmpty() && !file.isEmpty()) {
        const QString legacy = ini.get(QStringLiteral("game.language")).trimmed();
        if (!legacy.isEmpty()) {
            std::vector<std::string> t, sp;
            for (const QString& x : c.text) t.push_back(x.toStdString());
            for (const QString& x : c.speech) sp.push_back(x.toStdString());
            std::string tn, sn;
            ps2r::app::LegacyPair(t, sp, legacy.toStdString(), &tn, &sn);
            v = QString::fromStdString(text ? tn : sn);
        }
    }
    const QStringList& offered = text ? c.text : c.speech;
    if (offered.contains(v)) return v;
    return offered.contains(QStringLiteral("english")) ? QStringLiteral("english") : offered.value(0);
}

void LauncherApp::dropLanguagePackKeys() {
    const QString file = settingsFilePath();
    const QString mods = modsFolder();
    if (file.isEmpty() || mods.isEmpty()) return;
    QStringList packs;
    for (const ModEntry& e : ModList::read(mods)) if (ModList::isLanguagePack(e)) packs << e.id;
    if (packs.isEmpty()) return;
    IniSettings ini(file);
    QString err;
    for (const QString& id : packs)
        if (ini.has("mods." + id + ".enabled")) ini.remove("mods." + id + ".enabled", &err);
    QStringList order = ModList::parseOrder(ini.get(QStringLiteral("mods.order")));
    bool dropped = false;
    for (const QString& id : packs) dropped |= order.removeAll(id) > 0;
    if (dropped) {
        if (order.isEmpty()) ini.remove(QStringLiteral("mods.order"), &err);
        else ini.set(QStringLiteral("mods.order"), ModList::orderValue(order), &err);
    }
}

QString LauncherApp::version() const { return launcherVersion(); }

QString LauncherApp::engineCommit() const {
    // the engine commit the launcher was built from, stamped by the packaging into Resources/BUILD-ID when present
    for (const QString& p : {QCoreApplication::applicationDirPath() + "/../Resources/BUILD-ID", QCoreApplication::applicationDirPath() + "/BUILD-ID"}) {   // lint: not-ui
        QFile f(p);
        if (f.open(QIODevice::ReadOnly)) {
            for (const QString& l : QString::fromUtf8(f.readAll()).split('\n')) if (l.startsWith("engine")) return l.section(' ', 1).simplified().left(12);
        }
    }
    return QString();
}

}  // namespace ps2r::launcher
