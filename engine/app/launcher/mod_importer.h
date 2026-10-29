// app/launcher/mod_importer.h -- one import at a time: te::modimport::Import (the import
// library, engine/app/host/mod_import/mod_import.h; the stub with PS2R_LAUNCHER_IMPORT=stub) on a worker thread, its
// Progress reports delivered to the window's thread as progress(), Cancel as the library's cancellation, the result as
// finished(). Deleting the importer cancels a running import and waits for it.
#pragma once
#include <QFutureWatcher>
#include <QObject>
#include <QStringList>
#include <atomic>
#include <memory>
#include <string>
namespace ps2r::launcher {

struct ImportOutcome {
    bool ok = false;
    bool cancelled = false;
    QString artefact;      // the dropped or chosen file / folder
    QString id, name;      // the mod made (<mods>/<id>)
    QString error;         // the library's sentence when !ok
    bool replaced = false; // a mod of this id was in the mods folder and this one took its place (asked first)
    QStringList warnings, codeChanges, filesChanged, filesAdded;
};

class ModImporter : public QObject {
    Q_OBJECT
public:
    explicit ModImporter(QObject* parent = nullptr);
    ~ModImporter() override;
    bool running() const;
    QString artefact() const { return artefact_; }
    // Starts importing `artefact` against the disc `baseIso` into `modsDir`; false when an import is running already.
    bool start(const QString& artefact, const QString& baseIso, const QString& modsDir);
    void cancel();
    // The id an import of `artefact` would take (te::modimport::ModIdFor: read without importing; "" when it is not a mod)
    static std::string modIdFor(const QString& artefact);

signals:
    // What the library takes the artefact for ("a mod crate", "a patched disc image", ...), "" when it cannot tell
    void detected(const QString& kindLabel);
    void progress(const QString& stage, double fraction);
    void finished(const ps2r::launcher::ImportOutcome& outcome);

private:
    QFutureWatcher<ImportOutcome>* watcher_;
    std::shared_ptr<std::atomic_bool> cancel_;
    QString artefact_;
};

}  // namespace ps2r::launcher
