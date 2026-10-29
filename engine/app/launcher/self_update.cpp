// app/launcher/self_update.cpp -- see self_update.h.
#include "self_update.h"
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSettings>
#include <QTemporaryFile>
#include <QTextStream>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#if defined(Q_OS_MACOS)
#include <sys/mount.h>
#include <sys/xattr.h>
#endif
#if defined(Q_OS_WIN)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#endif
#include "app_data.h"
#include "launcher_version.h"
#include "release_download.h"
#include "title_manifest.h"
#include "zip_unpack.h"
namespace ps2r::launcher {

namespace {
constexpr int kCleanupTries = 60;   // 30 s of retries for a set-aside file the old program still holds (Windows)

// a name part made of [A-Za-z0-9._-] only (a tag, a version, a product)
QString safePart(const QString& s) {
    QString out;
    for (const QChar c : s) out += (c.isLetterOrNumber() && c.unicode() < 0x80) || c == '.' || c == '_' || c == '-' ? c : QChar('_');
    return out.isEmpty() ? QStringLiteral("x") : out;
}

// BUILD-ID's `built   2026-10-28T19:00:00Z` (every launcher's packaging writes it)
QDateTime builtTime(const QString& buildIdPath) {
    QFile f(buildIdPath);
    if (!f.open(QIODevice::ReadOnly)) return QDateTime();
    for (const QString& line : QString::fromUtf8(f.readAll()).split('\n')) {
        if (!line.startsWith(QLatin1String("built"))) continue;   // lint: not-ui
        const QDateTime t = QDateTime::fromString(line.mid(5).trimmed(), Qt::ISODate);
        if (t.isValid()) return t.toUTC();
    }
    return QDateTime();
}

// A folder (or file) may be written by this account: a probe file is made and removed
bool writableHere(const QString& dir) {
    QTemporaryFile probe(dir + "/.launcher-probe-XXXXXX");   // lint: not-ui
    return probe.open();
}

// The bundle is on a read-only volume (its disk image, mounted). statfs, not QStorageInfo: on macOS 10.15+ QStorageInfo
// answers "/" -- the sealed, read-only system volume -- for every path reached through the Data volume's firmlinks
// (/Users, /Applications, /private/var: measured on macOS 26.7.1, notes/launcher2.md), which would refuse every update.
bool readOnlyVolume(const QString& path) {
#if defined(Q_OS_MACOS)
    struct statfs st;
    return statfs(QFile::encodeName(path).constData(), &st) == 0 && (st.f_flags & MNT_RDONLY) != 0;
#else
    Q_UNUSED(path);
    return false;
#endif
}

bool removePath(const QString& path) {
    const QFileInfo fi(path);
    if (!fi.exists() && !fi.isSymLink()) return true;
    if (fi.isDir() && !fi.isSymLink()) return QDir(path).removeRecursively();
    return QFile::remove(path);
}

// macOS: the quarantine flag on a file ("" = none), for the update's log
QString quarantineOf(const QString& path) {
#if defined(Q_OS_MACOS)
    char value[512];
    const ssize_t n = getxattr(QFile::encodeName(path).constData(), "com.apple.quarantine", value, sizeof(value) - 1, 0, XATTR_NOFOLLOW);   // lint: not-ui
    return n > 0 ? QString::fromUtf8(value, int(n)) : QString();
#else
    Q_UNUSED(path);
    return QString();
#endif
}

// The arguments the relaunched launcher keeps: the home folder and the manifest it was told to use, and --no-scan
// (arguments, not the environment: macOS's `open` hands an app its arguments and never the caller's environment).
// `withHome`: --home is left out (the caller adds this launcher's own)
QStringList keptArguments(bool withHome) {
    QStringList kept;
    const QStringList args = QCoreApplication::arguments();
    for (int i = 1; i < args.size(); ++i) {
        if (args[i] == QLatin1String("--no-scan")) { kept << args[i]; continue; }   // lint: not-ui
        for (const char* withValue : {"--manifest", "--home"}) {   // lint: not-ui
            const bool home = qstrcmp(withValue, "--home") == 0;   // lint: not-ui
            if (args[i] == QLatin1String(withValue) && i + 1 < args.size()) {
                if (withHome || !home) kept << args[i] << args[i + 1];
                ++i;
                break;
            }
            if (args[i].startsWith(QLatin1String(withValue) + QLatin1Char('='))) {
                if (withHome || !home) kept << args[i];
                break;
            }
        }
    }
    return kept;
}
}  // namespace

SelfUpdate::SelfUpdate(AppData* appData, const TitleManifest* manifest, QObject* parent)
    : QObject(parent), appData_(appData), manifest_(manifest), download_(new ReleaseDownload(this)), running_(detect()) {
    connect(download_, &ReleaseDownload::listed, this, [this](const QJsonArray& releases) {
        if (state_ != Checking) return;
        LauncherRelease r;
        QString why;
        if (newer(releases, assetPattern(), running_.target, manifest_->sumsAsset(), running_, ReleaseDownload::useToken(), &r, &why)) {
            release_ = r;
            log(QStringLiteral("check: %1 is newer than %2 (%3)").arg(r.tag, running_.version, why));   // lint: not-ui
            setState(Available);
            if (startWhenFound_) start();
        } else {
            log(QStringLiteral("check: nothing newer than %1 (%2)").arg(running_.version, why));   // lint: not-ui
            setState(UpToDate);
        }
    });
    connect(download_, &ReleaseDownload::failed, this, [this](int failure) {
        if (state_ == Checking) {   // offline at start: nothing is offered this time, nothing is said
            log(QStringLiteral("check: the list could not be read"));   // lint: not-ui
            setState(Idle);
            return;
        }
        if (state_ != Downloading && state_ != Verifying) return;
        if (failure == ReleaseDownload::Damaged) failWith(Damaged, QStringLiteral("The download was damaged and has been deleted. Try again."));
        else if (failure == ReleaseDownload::NotWritable)
            failWith(NotWritable, QStringLiteral("The download could not be saved in %1.").arg(AppData::shortPath(appData_->downloadsDir())));
        else if (failure == ReleaseDownload::Cancelled) { log(QStringLiteral("download: stopped")); setState(Available); }   // lint: not-ui
        else failWith(Offline, QStringLiteral("The download did not finish. Check your connection and try again."));
    });
    connect(download_, &ReleaseDownload::progress, this, &SelfUpdate::progress);
    connect(download_, &ReleaseDownload::checking, this, [this] { setState(Verifying); });
    connect(download_, &ReleaseDownload::fetched, this, &SelfUpdate::place);
}

SelfUpdate::~SelfUpdate() = default;

// --- the running launcher -----------------------------------------------------------------------------------------

RunningLauncher SelfUpdate::detect() {
    RunningLauncher r;
    r.version = launcherVersion();
    r.target = ReleaseCatalogue::hostTarget();
    r.pid = QCoreApplication::applicationPid();
    r.home = AppData::homePath();
    const QString dir = QCoreApplication::applicationDirPath();
    const QFileInfo exe(QCoreApplication::applicationFilePath());
    r.program = exe.fileName();
    QString buildId;
    QString kind, path;
#if defined(Q_OS_MACOS)
    // <Name>.app/Contents/MacOS/<program>
    QDir d(dir);
    if (d.dirName() == QLatin1String("MacOS") && d.cdUp() && d.dirName() == QLatin1String("Contents") && d.cdUp() &&
        d.dirName().endsWith(QLatin1String(".app"))) {
        path = d.absolutePath();
        kind = QStringLiteral("app");
        buildId = path + "/Contents/Resources/BUILD-ID";   // lint: not-ui
        QString program;
        bundleInfo(path, &r.bundleId, &program);
    }
#elif defined(Q_OS_WIN)
    path = QDir::cleanPath(dir);
    kind = QStringLiteral("folder");
    buildId = path + "/BUILD-ID";   // lint: not-ui
#else
    const QString appimage = qEnvironmentVariable("APPIMAGE");   // lint: not-ui (set by the AppImage's runtime)
    if (!appimage.isEmpty() && QFileInfo(appimage).isFile()) {
        path = QFileInfo(appimage).absoluteFilePath();
        kind = QStringLiteral("appimage");
    }
    for (const QFileInfo& fi : QDir(dir + "/../share/doc").entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))   // lint: not-ui
        if (QFileInfo::exists(fi.absoluteFilePath() + "/BUILD-ID")) { buildId = fi.absoluteFilePath() + "/BUILD-ID"; break; }   // lint: not-ui
#endif
    r.made = buildId.isEmpty() ? QDateTime() : builtTime(buildId);
    // installed from a release = the packaging's BUILD-ID is there (a developer's copy has none: it is never replaced)
    if (!path.isEmpty() && QFileInfo::exists(buildId)) {
        r.kind = kind;
        r.path = path;
    }
    if (!r.made.isValid()) r.made = exe.lastModified().toUTC();
    return r;
}

bool SelfUpdate::bundleInfo(const QString& app, QString* bundleId, QString* program) {
    const QString plist = app + "/Contents/Info.plist";   // lint: not-ui
    if (!QFileInfo::exists(plist)) return false;
    QFile f(plist);
    if (!f.open(QIODevice::ReadOnly)) return false;
    // the XML property list the packaging writes: <key>K</key> followed by <string>V</string>
    const QString text = QString::fromUtf8(f.readAll());
    auto value = [&](const QString& key) {
        const QRegularExpression re(QStringLiteral("<key>%1</key>\\s*<string>([^<]*)</string>").arg(QRegularExpression::escape(key)));
        const QRegularExpressionMatch m = re.match(text);
        return m.hasMatch() ? m.captured(1).trimmed() : QString();
    };
    if (bundleId) *bundleId = value(QStringLiteral("CFBundleIdentifier"));   // lint: not-ui
    if (program) *program = value(QStringLiteral("CFBundleExecutable"));     // lint: not-ui
    return true;
}

// --- the release list, as data ------------------------------------------------------------------------------------

QString SelfUpdate::baseTag(const QString& version, int* commitsAfter) {
    QString v = version.trimmed();
    if (v.endsWith(QLatin1String("-dirty"))) v.chop(6);   // lint: not-ui
    static const QRegularExpression described(QStringLiteral("^(.+)-(\\d+)-g[0-9a-f]{4,40}$"));
    static const QRegularExpression hashOnly(QStringLiteral("^[0-9a-f]{7,40}$"));
    const QRegularExpressionMatch m = described.match(v);
    if (m.hasMatch()) {
        if (commitsAfter) *commitsAfter = m.captured(2).toInt();
        return m.captured(1);
    }
    if (v.isEmpty() || hashOnly.match(v).hasMatch()) {   // `git describe --always` without a tag: no place in any list
        if (commitsAfter) *commitsAfter = -1;
        return QString();
    }
    if (commitsAfter) *commitsAfter = 0;
    return v;
}

bool SelfUpdate::newer(const QJsonArray& releases, const QString& assetPattern, const QString& target, const QString& sumsName,
                       const RunningLauncher& running, bool apiUrls, LauncherRelease* out, QString* why) {
    auto say = [&](const QString& s) {
        if (why) *why = s;
    };
    if (assetPattern.isEmpty()) {
        say(QStringLiteral("no launcher file is published for %1").arg(target));   // lint: not-ui
        return false;
    }
    // the running launcher's place in the list (the API's order: newest first): its version as a tag (a hand-made list
    // may name describe strings), else the tag it was made after
    const QString base = baseTag(running.version);
    int runningAt = -1;
    for (int pass = 0; pass < 2 && runningAt < 0; ++pass) {
        const QString want = pass == 0 ? running.version : base;
        if (want.isEmpty()) continue;
        for (int i = 0; i < releases.size(); ++i)
            if (releases[i].toObject().value("tag_name").toString() == want) { runningAt = i; break; }
    }
    for (int i = 0; i < releases.size(); ++i) {
        const QJsonObject rel = releases[i].toObject();
        if (rel.value("draft").toBool() || rel.value("prerelease").toBool()) continue;
        const QString tag = rel.value("tag_name").toString();
        if (tag.isEmpty()) continue;
        const QString name = ReleaseCatalogue::assetName(assetPattern, tag, target);
        LauncherRelease c;
        for (const QJsonValue& v : rel.value("assets").toArray()) {
            const QJsonObject a = v.toObject();
            ReleaseAsset r;
            r.name = a.value("name").toString();
            r.url = QUrl(apiUrls ? a.value("url").toString() : a.value("browser_download_url").toString());
            r.bytes = qint64(a.value("size").toDouble());
            if (r.name == name) c.asset = r;
            if (r.name == sumsName) c.sums = r;
        }
        if (c.asset.name.isEmpty() || !c.asset.url.isValid() || c.sums.name.isEmpty() || !c.sums.url.isValid()) continue;
        c.tag = tag;
        c.published = QDateTime::fromString(rel.value("published_at").toString(), Qt::ISODate);
        c.page = QUrl(rel.value("html_url").toString());
        // the newest release carrying the file decides
        if (runningAt >= 0) {
            if (i < runningAt) {
                say(QStringLiteral("%1 is listed before %2, newest first").arg(tag, releases[runningAt].toObject().value("tag_name").toString()));   // lint: not-ui
                if (out) *out = c;
                return true;
            }
            say(QStringLiteral("%1 is the newest with a launcher file and the running %2 is not older").arg(tag, running.version));   // lint: not-ui
            return false;
        }
        if (!c.published.isValid() || !running.made.isValid()) {
            say(QStringLiteral("%1 is not in the list and %2 has no date to compare").arg(running.version, tag));   // lint: not-ui
            return false;
        }
        if (c.published > running.made) {
            say(QStringLiteral("%1 is not in the list; %2 was published %3, after this launcher was made %4").arg(running.version, tag,   // lint: not-ui
                                                                                                                c.published.toString(Qt::ISODate),
                                                                                                                running.made.toString(Qt::ISODate)));
            if (out) *out = c;
            return true;
        }
        say(QStringLiteral("%1 is not in the list; %2 was published %3, before this launcher was made %4").arg(running.version, tag,   // lint: not-ui
                                                                                                             c.published.toString(Qt::ISODate),
                                                                                                             running.made.toString(Qt::ISODate)));
        return false;
    }
    say(QStringLiteral("no published release carries %1").arg(ReleaseCatalogue::assetName(assetPattern, QStringLiteral("<tag>"), target)));   // lint: not-ui
    return false;
}

QString SelfUpdate::assetPattern() const { return manifest_->launcherAssetPattern(running_.target); }

// --- the update ---------------------------------------------------------------------------------------------------

void SelfUpdate::setState(State state) {
    state_ = state;
    if (state != Failed) {
        problem_ = NoProblem;
        problemText_.clear();
    }
    emit stateChanged();
}

void SelfUpdate::failWith(Problem problem, const QString& text) {
    log(QStringLiteral("failed: %1").arg(text));   // lint: not-ui
    state_ = Failed;
    problem_ = problem;
    problemText_ = text;
    emit stateChanged();
}

void SelfUpdate::log(const QString& line) {
    if (logPath_.isEmpty()) {
        QDir().mkpath(appData_->logsDir());
        logPath_ = appData_->logPath(QStringLiteral("launcher-update"));   // lint: not-ui
    }
    QFile f(logPath_);
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    QTextStream(&f) << QDateTime::currentDateTimeUtc().toString(Qt::ISODateWithMs) << ' ' << line << '\n';
}

void SelfUpdate::check() {
    if (state_ == Checking || state_ == Downloading || state_ == Verifying || state_ == Replacing || state_ == Restarting) return;
    const QString chosen = appData_->value("releases_url").toString();   // lint: not-ui
    const QString url = chosen.isEmpty() ? manifest_->releasesUrl() : chosen;
    log(QStringLiteral("check: %1 (%2, %3 at %4, made %5, process %6) against %7").arg(running_.version, running_.target,   // lint: not-ui
                                                                                      running_.kind.isEmpty() ? QStringLiteral("not installed") : running_.kind,
                                                                                      running_.path.isEmpty() ? QStringLiteral("-") : running_.path,
                                                                                      running_.made.toString(Qt::ISODate))
            .arg(running_.pid).arg(url));
    setState(Checking);
    download_->fetchList(url);
}

SelfUpdate::Problem SelfUpdate::cannotReplace(QString* text) const {
    auto out = [&](Problem p, const QString& t) {
        if (text) *text = t;
        return p;
    };
    const QString product = manifest_->product();
    if (gameRunning_ && gameRunning_()) return out(GameRunning, QStringLiteral("Close the game first: the launcher opens again to update."));
    if (running_.kind.isEmpty() || running_.path.isEmpty())
        return out(NotInstalled, QStringLiteral("This copy of the launcher cannot replace itself. Download the new one from its page."));
    if (running_.kind == QLatin1String("app")) {
        if (running_.path.contains(QLatin1String("/AppTranslocation/")))   // lint: not-ui (macOS's temporary copy of a just-downloaded app)
            return out(Translocated, QStringLiteral("macOS is running %1 from a temporary copy. Move %1 to your Applications folder, open it from "
                                                    "there and update again.").arg(product));
        if (readOnlyVolume(running_.path))
            return out(DiskImage, QStringLiteral("%1 is running from its disk image. Drag it to your Applications folder, open it from there and "
                                                 "update again.").arg(product));
    }
    const QString where = running_.kind == QLatin1String("folder") ? running_.path : QFileInfo(running_.path).absolutePath();
    if (!writableHere(where) || !QFileInfo(running_.path).isWritable())
        return out(NotWritable, QStringLiteral("The launcher is in %1, which this account cannot change. Download the new one and put it there "
                                               "yourself.").arg(AppData::shortPath(where)));
    return out(NoProblem, QString());
}

void SelfUpdate::start() {
    if ((state_ != Available && state_ != Failed) || release_.tag.isEmpty()) return;
    logPath_.clear();   // one log per attempt
    QString why;
    const Problem p = cannotReplace(&why);
    log(QStringLiteral("start: %1 -> %2 (%3)").arg(running_.version, release_.tag, release_.asset.name));   // lint: not-ui
    if (p != NoProblem) {
        failWith(p, why);
        return;
    }
    dismissed_ = false;
    setState(Downloading);
    download_->fetchChecked(release_.asset, release_.sums, appData_->downloadsDir());
}

void SelfUpdate::dismiss() {
    dismissed_ = true;
    emit stateChanged();
}

QString SelfUpdate::statusText() const {
    switch (state_) {
    case Checking: return QStringLiteral("Looking for a newer launcher…");
    case UpToDate: return QStringLiteral("This is the newest launcher.");
    case Available: {
        QString t = QStringLiteral("A new launcher is ready: %1.").arg(release_.tag);
        QString why;
        if (cannotReplace(&why) != NoProblem) t += " " + why;
        return t;
    }
    case Downloading: return QStringLiteral("Downloading the launcher %1…").arg(release_.tag);
    case Verifying: return QStringLiteral("Checking the download…");
    case Replacing: return QStringLiteral("Putting the new launcher in place…");
    case Restarting: return QStringLiteral("The launcher opens again in a moment.");
    case Failed: return QStringLiteral("The launcher was not updated. %1").arg(problemText_);
    case Idle: break;
    }
    return QString();
}

QUrl SelfUpdate::downloadPage() const {
    if (release_.page.isValid() && !release_.page.isEmpty()) return release_.page;
    const QString page = manifest_->pageUrl();
    return page.isEmpty() ? QUrl() : QUrl(page + "/releases/latest");   // lint: not-ui
}

qint64 SelfUpdate::received() const { return download_->received(); }
qint64 SelfUpdate::total() const { return download_->total(); }
double SelfUpdate::rate() const { return download_->rate(); }

QString SelfUpdate::aside(const QString& name) const {
    // the running copy set aside beside itself under a hidden name; the new launcher removes it
    const QFileInfo fi(name);
    const QString suffix = fi.suffix().isEmpty() ? QString() : "." + fi.suffix();
    return "." + safePart(fi.completeBaseName()) + "-replaced-" + safePart(running_.version) + suffix;   // lint: not-ui
}

void SelfUpdate::saveRecord(const QStringList& setAside, const QString& staging) {
    QJsonObject rec;
    rec["from"] = running_.version;
    rec["to"] = release_.tag;
    rec["kind"] = running_.kind;
    rec["path"] = running_.path;
    rec["aside"] = QJsonArray::fromStringList(setAside);
    rec["staging"] = staging;
    rec["pid"] = double(running_.pid);
    rec["at"] = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    appData_->setValue("launcher_update", rec);   // lint: not-ui
    appData_->save();
}

void SelfUpdate::place(const QString& file) {
    if (state_ != Verifying && state_ != Downloading) return;
    log(QStringLiteral("download: %1 (%2 bytes) matches SHA256SUMS").arg(QFileInfo(file).fileName()).arg(QFileInfo(file).size()));   // lint: not-ui
    log(QStringLiteral("download: quarantine flag %1").arg(quarantineOf(file).isEmpty() ? QStringLiteral("none") : quarantineOf(file)));   // lint: not-ui
    setState(Replacing);
    QString error;
    Problem problem = Broken;
    bool ok = false;
    if (running_.kind == QLatin1String("app")) ok = placeApp(file, &error, &problem);
    else if (running_.kind == QLatin1String("folder")) ok = placeFolder(file, &error, &problem);
    else if (running_.kind == QLatin1String("appimage")) ok = placeAppImage(file, &error, &problem);
    QFile::remove(file);
    if (!ok) {
        failWith(problem, error);
        return;
    }
    log(QStringLiteral("placed: %1 is %2 now").arg(running_.path, release_.tag));   // lint: not-ui
    setState(Restarting);
    QString err;
    if (!relaunchNow(&err)) {
        failWith(Broken, QStringLiteral("The new launcher is in place but did not open by itself (%1): quit and open it again.").arg(err));
        return;
    }
    log(QStringLiteral("relaunch: started; this launcher quits"));   // lint: not-ui
    emit quitRequested();
}

bool SelfUpdate::placeApp(const QString& zip, QString* error, Problem* problem) {
    const QFileInfo bundle(running_.path);
    const QString parent = bundle.absolutePath();
    const QString staging = parent + "/." + safePart(bundle.completeBaseName()) + "-update-" + safePart(release_.tag);   // lint: not-ui
    removePath(staging);
    QString err;
    if (!unpackZip(zip, staging, &err)) {
        removePath(staging);
        *problem = Broken;
        *error = QStringLiteral("The new launcher could not be unpacked (%1). Nothing was changed.").arg(err);
        return false;
    }
    QStringList apps;
    for (const QFileInfo& fi : QDir(staging).entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (fi.fileName().endsWith(QLatin1String(".app"))) apps << fi.absoluteFilePath();
    QString id, program;
    if (apps.size() != 1 || !bundleInfo(apps.first(), &id, &program) || (!running_.bundleId.isEmpty() && id != running_.bundleId) ||
        !QFileInfo(apps.first() + "/Contents/MacOS/" + program).isExecutable()) {
        log(QStringLiteral("check: %1 app(s) in the download, bundle id %2 (want %3), program %4").arg(apps.size()).arg(id, running_.bundleId, program));   // lint: not-ui
        removePath(staging);
        *problem = Broken;
        *error = QStringLiteral("The download is not this launcher. Nothing was changed.");
        return false;
    }
    const QString staged = apps.first();
    log(QStringLiteral("unpacked: %1 (bundle id %2); quarantine flag %3").arg(staged, id,   // lint: not-ui
                                                                             quarantineOf(staged).isEmpty() ? QStringLiteral("none") : quarantineOf(staged)));
    // the code signature must hold before the running copy is touched (a bundle that does not check would not open)
    if (QFileInfo::exists(QStringLiteral("/usr/bin/codesign"))) {   // lint: not-ui
        QProcess cs;
        cs.setProcessChannelMode(QProcess::MergedChannels);
        cs.start(QStringLiteral("/usr/bin/codesign"), {QStringLiteral("--verify"), QStringLiteral("--deep"), QStringLiteral("--strict"), staged});   // lint: not-ui
        const bool done = cs.waitForFinished(180000);
        const QString said = QString::fromUtf8(cs.readAll()).trimmed();
        log(QStringLiteral("codesign --verify --deep --strict: exit %1 %2").arg(done ? cs.exitCode() : -1).arg(said));   // lint: not-ui
        if (!done || cs.exitStatus() != QProcess::NormalExit || cs.exitCode() != 0) {
            removePath(staging);
            *problem = Broken;
            *error = QStringLiteral("The new launcher did not pass its check. Nothing was changed.");
            return false;
        }
    }
    const QString set = parent + "/" + aside(bundle.fileName());
    saveRecord({set}, staging);
    QString why;
    if (!swapIn(running_.path, staged, set, &why)) {
        appData_->remove("launcher_update");   // lint: not-ui
        appData_->save();
        removePath(staging);
        *problem = NotWritable;
        *error = QStringLiteral("The launcher could not be replaced (%1). Nothing was changed.").arg(why);
        return false;
    }
    removePath(staging);
    log(QStringLiteral("swapped: %1 -> %2; %3 -> %1").arg(running_.path, set, staged));   // lint: not-ui
    return true;
}

bool SelfUpdate::placeFolder(const QString& zip, QString* error, Problem* problem) {
    const QString folder = running_.path;
    const QString staging = folder + "/.update-" + safePart(release_.tag);   // lint: not-ui
    removePath(staging);
    QString err;
    if (!unpackZip(zip, staging, &err)) {
        removePath(staging);
        *problem = Broken;
        *error = QStringLiteral("The new launcher could not be unpacked (%1). Nothing was changed.").arg(err);
        return false;
    }
    // the zip holds one folder (the product's) with the program in it
    QString root = staging;
    const QStringList dirs = QDir(staging).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    if (!QFileInfo::exists(staging + "/" + running_.program) && dirs.size() == 1) root = staging + "/" + dirs.first();
    if (!QFileInfo::exists(root + "/" + running_.program)) {
        removePath(staging);
        *problem = Broken;
        *error = QStringLiteral("The download is not this launcher. Nothing was changed.");
        return false;
    }
    QStringList setAside;
    if (!replaceFiles(root, folder, QStringLiteral(".replaced"), &setAside, &err)) {   // lint: not-ui
        removePath(staging);
        *problem = NotWritable;
        *error = QStringLiteral("The launcher could not be replaced (%1). Nothing was changed.").arg(err);
        return false;
    }
    removePath(staging);
    saveRecord(setAside, QString());
    log(QStringLiteral("replaced: %1 files set aside in %2").arg(setAside.size()).arg(folder));   // lint: not-ui
    return true;
}

bool SelfUpdate::placeAppImage(const QString& file, QString* error, Problem* problem) {
    const QFileInfo cur(running_.path);
    QFile f(file);
    if (!f.open(QIODevice::ReadOnly) || f.read(4) != QByteArray("\x7f" "ELF", 4)) {
        *problem = Broken;
        *error = QStringLiteral("The download is not this launcher. Nothing was changed.");
        return false;
    }
    f.close();
    // beside the running file (the same file system: the rename is one step), executable, then swapped in
    const QString next = cur.absolutePath() + "/." + cur.fileName() + ".new";   // lint: not-ui
    QFile::remove(next);
    if (!QFile::copy(file, next) ||
        !QFile(next).setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner | QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                    QFileDevice::ExeGroup | QFileDevice::ReadOther | QFileDevice::ExeOther)) {
        QFile::remove(next);
        *problem = NotWritable;
        *error = QStringLiteral("The new launcher could not be written beside %1. Nothing was changed.").arg(cur.fileName());
        return false;
    }
    const QString set = cur.absolutePath() + "/" + aside(cur.fileName());
    saveRecord({set}, QString());
    QString why;
    if (!swapIn(running_.path, next, set, &why)) {
        appData_->remove("launcher_update");   // lint: not-ui
        appData_->save();
        QFile::remove(next);
        *problem = NotWritable;
        *error = QStringLiteral("The launcher could not be replaced (%1). Nothing was changed.").arg(why);
        return false;
    }
    return true;
}

bool SelfUpdate::relaunchNow(QString* error) {
    if (relaunch_) return relaunch_(running_, error);
    qint64 pid = 0;
    bool ok = false;
#if defined(Q_OS_MACOS)
    // a shell waits until this process has exited (at most a minute), then LaunchServices opens the new bundle as the
    // Finder would; what this launcher was started with (--home, --manifest, --no-scan) goes
    // as arguments: `open` hands an app its arguments, never the caller's environment (measured on macOS 26.7.1)
    // and this launcher's HOME always, as --home: LaunchServices starts an app with the account's own HOME, so a launcher
    // that runs with another (a test's, a terminal's) keeps its data folder across the update
    QStringList open{QStringLiteral("/usr/bin/open"), QStringLiteral("-n"), running_.path, QStringLiteral("--args"),   // lint: not-ui
                     QStringLiteral("--after-update"), QStringLiteral("--home"), running_.home};   // lint: not-ui
    open << keptArguments(false);
    const QString wait = QStringLiteral("pid=$1; shift; n=0; while kill -0 \"$pid\" 2>/dev/null && [ \"$n\" -lt 600 ]; do n=$((n+1)); sleep 0.1; done; exec \"$@\"");   // lint: not-ui
    QStringList args{QStringLiteral("-c"), wait, QStringLiteral("sh"), QString::number(running_.pid)};   // lint: not-ui
    args += open;
    ok = QProcess::startDetached(QStringLiteral("/bin/sh"), args, QString(), &pid);   // lint: not-ui
#elif defined(Q_OS_WIN)
    // started now, it waits (--wait-for) until this process has exited before it reads anything: this launcher's last
    // save of launcher.json (the window's close) cannot land after the new one's clean-up
    const QStringList next = QStringList{QStringLiteral("--after-update"), QStringLiteral("--wait-for"), QString::number(running_.pid)} + keptArguments(true);   // lint: not-ui
    ok = QProcess::startDetached(running_.path + "/" + running_.program, next, running_.path, &pid);
#else
    const QStringList next = QStringList{QStringLiteral("--after-update"), QStringLiteral("--wait-for"), QString::number(running_.pid)} + keptArguments(true);   // lint: not-ui
    ok = QProcess::startDetached(running_.path, next, QFileInfo(running_.path).absolutePath(), &pid);
#endif
    if (!ok && error) *error = QStringLiteral("the new launcher could not be started");
    if (ok) log(QStringLiteral("relaunch: process %1 waits for %2").arg(pid).arg(running_.pid));   // lint: not-ui
    return ok;
}

void SelfUpdate::finishAfterRelaunch() {
    const QJsonObject rec = appData_->value("launcher_update").toObject();   // lint: not-ui
    if (rec.isEmpty()) return;
    const QString path = QDir::cleanPath(rec.value("path").toString());
    if (path.isEmpty() || path != QDir::cleanPath(running_.path)) return;   // another copy of the launcher: its record stays
    const QString kind = rec.value("kind").toString();
    const QString to = rec.value("to").toString();
    if (finishedText_.isEmpty()) {
        updatedOk_ = running_.version == to;
        finishedText_ = updatedOk_ ? QStringLiteral("The launcher was updated to %1.").arg(to)
                                   : QStringLiteral("The launcher was not updated: this is still %1.").arg(running_.version);
        log(QStringLiteral("after the relaunch: %1 (recorded %2 -> %3), process %4, HOME %5").arg(running_.version, rec.value("from").toString(), to)   // lint: not-ui
                .arg(running_.pid).arg(running_.home));
    }
    // only what this launcher set aside, where it set it: beside the bundle / AppImage, or inside the folder
    const QString parent = QFileInfo(path).absolutePath();
    QStringList left;
    QStringList paths;
    for (const QJsonValue& v : rec.value("aside").toArray()) paths << QDir::cleanPath(v.toString());
    const QString staging = QDir::cleanPath(rec.value("staging").toString());
    if (!staging.isEmpty()) paths << staging;
    for (const QString& p : paths) {
        const QFileInfo fi(p);
        const bool beside = (kind == QLatin1String("app") || kind == QLatin1String("appimage")) && fi.absolutePath() == parent &&
                            fi.fileName().startsWith('.') && (fi.fileName().contains(QLatin1String("-replaced-")) || fi.fileName().contains(QLatin1String("-update-")));   // lint: not-ui
        const bool inside = kind == QLatin1String("folder") && p.startsWith(path + "/") && p.endsWith(QLatin1String(".replaced"));   // lint: not-ui
        if (!beside && !inside) {
            log(QStringLiteral("after the relaunch: %1 is not one of the launcher's own: left alone").arg(p));   // lint: not-ui
            continue;
        }
        if (!removePath(p)) left << p;
    }
    if (!left.isEmpty() && tries_++ < kCleanupTries) {   // the old program may still hold its files for a moment (Windows)
        QTimer::singleShot(500, this, &SelfUpdate::finishAfterRelaunch);
        emit stateChanged();
        return;
    }
    if (!left.isEmpty()) log(QStringLiteral("after the relaunch: could not remove %1").arg(left.join(", ")));   // lint: not-ui
    else log(QStringLiteral("after the relaunch: the replaced copy was removed"));   // lint: not-ui
    appData_->remove("launcher_update");   // lint: not-ui
    appData_->save();
    emit stateChanged();
}

// --- the file steps -----------------------------------------------------------------------------------------------

bool SelfUpdate::waitForExit(qint64 pid, int timeoutMs) {
    if (pid <= 0 || pid == QCoreApplication::applicationPid()) return true;
#if defined(Q_OS_WIN)
    HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, DWORD(pid));
    if (!process) return true;   // gone already
    const DWORD r = WaitForSingleObject(process, DWORD(qMax(0, timeoutMs)));
    CloseHandle(process);
    return r == WAIT_OBJECT_0;
#else
    // the old launcher started this one detached: it is not this process's child, so it never lingers as this one's
    // zombie -- its own parent (the desktop, launchd, init) collects it
    QElapsedTimer t;
    t.start();
    while (::kill(pid_t(pid), 0) == 0 || errno == EPERM) {
        if (t.elapsed() >= timeoutMs) return false;
        QThread::msleep(50);
    }
    return true;
#endif
}
bool SelfUpdate::swapIn(const QString& current, const QString& staged, const QString& asidePath, QString* error) {
    if (!removePath(asidePath)) {   // a leftover of an earlier update
        if (error) *error = QStringLiteral("%1 is in the way").arg(QDir::toNativeSeparators(asidePath));
        return false;
    }
    QDir fs;
    if (!fs.rename(current, asidePath)) {
        if (error) *error = QStringLiteral("%1 could not be moved").arg(QDir::toNativeSeparators(current));
        return false;
    }
    if (!fs.rename(staged, current)) {
        fs.rename(asidePath, current);   // undone: the running copy is back where it was
        if (error) *error = QStringLiteral("the new copy could not be moved to %1").arg(QDir::toNativeSeparators(current));
        return false;
    }
    return true;
}

bool SelfUpdate::replaceFiles(const QString& from, const QString& into, const QString& suffix, QStringList* setAside, QString* error) {
    struct Done {
        QString dest, old;   // old = "" when nothing was there
    };
    QVector<Done> done;
    auto undo = [&] {
        for (int i = int(done.size()) - 1; i >= 0; --i) {
            QFile::remove(done[i].dest);
            if (!done[i].old.isEmpty()) QFile::rename(done[i].old, done[i].dest);
        }
    };
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::System, QDirIterator::Subdirectories);
    QStringList files;
    while (it.hasNext()) files << it.next();
    files.sort();
    for (const QString& src : files) {
        const QString rel = QDir(from).relativeFilePath(src);
        const QString dest = into + "/" + rel;
        if (!QDir().mkpath(QFileInfo(dest).absolutePath())) {
            undo();
            if (error) *error = QStringLiteral("%1 could not be made").arg(QDir::toNativeSeparators(QFileInfo(dest).absolutePath()));
            return false;
        }
        Done d{dest, QString()};
        if (QFileInfo::exists(dest) || QFileInfo(dest).isSymLink()) {
            d.old = dest + suffix;
            QFile::remove(d.old);
            if (!QFile::rename(dest, d.old)) {
                undo();
                if (error) *error = QStringLiteral("%1 could not be set aside").arg(QDir::toNativeSeparators(dest));
                return false;
            }
        }
        if (!QFile::rename(src, dest) && !QFile::copy(src, dest)) {
            if (!d.old.isEmpty()) QFile::rename(d.old, dest);
            undo();
            if (error) *error = QStringLiteral("%1 could not be written").arg(QDir::toNativeSeparators(dest));
            return false;
        }
        done << d;
    }
    if (setAside)
        for (const Done& d : done)
            if (!d.old.isEmpty()) *setAside << d.old;
    return true;
}

}  // namespace ps2r::launcher
