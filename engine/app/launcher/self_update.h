// app/launcher/self_update.h -- the launcher replacing ITSELF with a newer published release (
//; the game's own update stays ReleaseInstaller's Update button, unchanged).
//
//   check    at start and from About: the releases list (the same releases_url as the game's), the newest published
//            release carrying THIS computer's launcher file (title.json release.launcher_assets) and SHA256SUMS; it is
//            offered only when it is newer than the running launcher -- by the list's order (the API's: newest first)
//            when the running version's tag is in it, else by its publication date against the moment the running
//            launcher was made (BUILD-ID's `built`). Never a text compare of versions.
//   start    the file downloaded into the launcher's Downloads/ and checked against SHA256SUMS (ReleaseDownload), then
//            put in place WITHOUT elevated rights, by the kind of install:
//              app       macOS: unpacked into a hidden folder beside the bundle (same volume), the new bundle checked (the
//                        same bundle id, its program, `codesign --verify`), the running bundle renamed aside and the new
//                        one renamed in (rolled back on any failure); a shell waits for this process to exit and opens
//                        the new bundle with LaunchServices (`open -n <bundle> --args --after-update`, plus the --home,
//                        --manifest and --no-scan this launcher was started with: `open` passes arguments, not the
//                        environment);
//              folder    Windows: unpacked into a hidden folder inside the launcher's folder; every file that is replaced
//                        is renamed to <name>.replaced first (Windows lets a running program and its libraries be renamed,
//                        not overwritten), the new one moved in; the new program started with --wait-for <this process>
//                        (it does nothing until this one has exited: the old launcher's last save of launcher.json
//                        cannot land after the new one's clean-up); quits;
//              appimage  Linux ($APPIMAGE): the new AppImage beside the old one, the old renamed aside, the new renamed in;
//                        started with --wait-for, as on Windows.
//            The new launcher (started with --after-update, or simply opened later) removes what was set aside -- only the
//            paths this launcher recorded in launcher.json -- and says once "The launcher was updated to <tag>."
//   cannot   not writable (e.g. /Applications owned by another account), run from its disk image or from macOS's
//            temporary copy of a just-downloaded app, or not installed from a release (no BUILD-ID: a developer's copy):
//            the offer says why and gives the release's page instead (Download it).
// Every step is written to Logs/launcher-update-<time>.log.
#pragma once
#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QUrl>
#include <functional>
#include "release_catalogue.h"
namespace ps2r::launcher {
class AppData;
class ReleaseDownload;
class TitleManifest;

struct LauncherRelease {
    QString tag;
    QDateTime published;
    QUrl page;              // the release's page (the API's html_url): where a player downloads it by hand
    ReleaseAsset asset;     // this computer's launcher file
    ReleaseAsset sums;      // SHA256SUMS
};

struct RunningLauncher {
    QString version;        // launcherVersion(): the tree's describe at packaging time
    QDateTime made;         // BUILD-ID's `built` (UTC), else the program file's time
    QString target;         // macos-arm64 / macos-x86_64 / windows-x64 / linux-x86_64 / linux-aarch64
    QString kind;           // "app" | "folder" | "appimage" | "" (not installed from a release: it cannot replace itself)
    QString path;           // the bundle (.app), the folder holding the program, or the AppImage file
    QString program;        // the program's file name (inside Contents/MacOS for an app)
    QString bundleId;       // macOS: CFBundleIdentifier
    QString home;           // HOME (USERPROFILE) as this launcher uses it (--home sets it): written to the update's log
    qint64 pid = 0;
};

class SelfUpdate : public QObject {
    Q_OBJECT
public:
    enum State { Idle, Checking, UpToDate, Available, Downloading, Verifying, Replacing, Restarting, Failed };
    enum Problem { NoProblem, Offline, Damaged, NotWritable, DiskImage, Translocated, NotInstalled, GameRunning, Broken };
    // the relaunch: starts `next` (the replaced launcher) once this process has exited; tests record it instead
    using Relaunch = std::function<bool(const RunningLauncher& next, QString* error)>;

    SelfUpdate(AppData* appData, const TitleManifest* manifest, QObject* parent = nullptr);
    ~SelfUpdate() override;

    // --- the running launcher -------------------------------------------------------------------------------------
    static RunningLauncher detect();
    void setRunning(const RunningLauncher& running) { running_ = running; }   // tests: a launcher elsewhere
    const RunningLauncher& running() const { return running_; }
    void setRelaunch(Relaunch relaunch) { relaunch_ = std::move(relaunch); }
    void setGameRunning(std::function<bool()> gameRunning) { gameRunning_ = std::move(gameRunning); }

    // --- the release list, as data (no network) -------------------------------------------------------------------
    // "v0.2.1-14-g1a2b3c4-dirty" -> "v0.2.1" (14 commits after it); a plain tag -> itself (0); a bare hash -> "" (-1)
    static QString baseTag(const QString& version, int* commitsAfter = nullptr);
    // The newest release (drafts and prereleases skipped) carrying the target's launcher file and `sumsName`, when it is
    // newer than `running` (see the header's comment); *why says which rule decided (logged). False: nothing newer.
    static bool newer(const QJsonArray& releases, const QString& assetPattern, const QString& target, const QString& sumsName,
                      const RunningLauncher& running, bool apiUrls, LauncherRelease* out, QString* why = nullptr);

    // --- the update -----------------------------------------------------------------------------------------------
    void check();                // Checking -> Available | UpToDate; back to Idle when offline (silent)
    void start();                // Available -> Downloading -> Verifying -> Replacing -> Restarting (quitRequested) | Failed
    void setStartWhenFound(bool on) { startWhenFound_ = on; }   // --update-launcher: start() as soon as check() finds one
    void dismiss();              // "Not now": the Games page's offer is hidden until the next start
    // Why this launcher cannot replace itself where it is (NoProblem: it can); `text` the sentence
    Problem cannotReplace(QString* text = nullptr) const;
    // The new launcher's first start after an update: what the old one set aside (as launcher.json records it) is removed
    // and the update is told once. Runs at every start; without a record it does nothing.
    void finishAfterRelaunch();

    State state() const { return state_; }
    Problem problem() const { return problem_; }
    QString problemText() const { return problemText_; }
    QString statusText() const;           // one sentence for the Games page's and About's line
    LauncherRelease release() const { return release_; }
    bool dismissed() const { return dismissed_; }
    QString finishedText() const { return finishedText_; }   // "The launcher was updated to v0.2.1." after a relaunch
    bool updatedOk() const { return updatedOk_; }             // ... and it is the version the update recorded
    QUrl downloadPage() const;            // the release's page, else the project's latest-release page
    qint64 received() const;
    qint64 total() const;
    double rate() const;
    QString logPath() const { return logPath_; }
    QString assetPattern() const;         // title.json release.launcher_assets for this computer ("" = none)

    // --- the file steps (public for the tests; the paths are the caller's) ------------------------------------------
    // `current` renamed to `aside`, then `staged` renamed to `current`; the first undone when the second fails
    static bool swapIn(const QString& current, const QString& staged, const QString& aside, QString* error);
    // every file of `from` moved into `into` (same volume); a file already there renamed to <file><suffix> first and
    // listed in *setAside; on a failure everything done is undone
    static bool replaceFiles(const QString& from, const QString& into, const QString& suffix, QStringList* setAside, QString* error);
    // the bundle's identity for the check before the swap (macOS Info.plist: CFBundleIdentifier, CFBundleExecutable)
    static bool bundleInfo(const QString& app, QString* bundleId, QString* program);
    // --wait-for: true once process `pid` has exited (at once when it is not running), false after `timeoutMs`
    // (Windows: the process handle; elsewhere kill(pid, 0) -- the old launcher is never this one's child)
    static bool waitForExit(qint64 pid, int timeoutMs);

signals:
    void stateChanged();
    void progress(qint64 received, qint64 total, double bytesPerSecond);
    void quitRequested();   // the new launcher opens once this one has quit

private:
    void setState(State state);
    void failWith(Problem problem, const QString& text);
    void log(const QString& line);
    void place(const QString& file);
    bool placeApp(const QString& zip, QString* error, Problem* problem);
    bool placeFolder(const QString& zip, QString* error, Problem* problem);
    bool placeAppImage(const QString& file, QString* error, Problem* problem);
    bool relaunchNow(QString* error);
    void saveRecord(const QStringList& aside, const QString& staging);
    QString aside(const QString& name) const;

    AppData* appData_;
    const TitleManifest* manifest_;
    ReleaseDownload* download_;
    RunningLauncher running_;
    Relaunch relaunch_;
    std::function<bool()> gameRunning_;
    State state_ = Idle;
    Problem problem_ = NoProblem;
    QString problemText_, finishedText_, logPath_;
    LauncherRelease release_;
    bool startWhenFound_ = false, dismissed_ = false, updatedOk_ = false;
    int tries_ = 0;   // finishAfterRelaunch's retries
};

}  // namespace ps2r::launcher
