// app/launcher/mod_importer.cpp -- the import on a worker thread.
#include "mod_importer.h"
#include <QFile>
#include <QFileInfo>
#include <QtConcurrent/QtConcurrentRun>
#include "app/host/mod_import/mod_import.h"
namespace ps2r::launcher {

namespace {
QStringList strings(const std::vector<std::string>& v) {
    QStringList out;
    for (const std::string& s : v) out << QString::fromStdString(s);
    return out;
}
std::string localPath(const QString& path) { return QFile::encodeName(path).toStdString(); }

QString kindLabel(te::modimport::Kind kind) {
    switch (kind) {
    case te::modimport::Kind::ModCrate: return QStringLiteral("a mod crate");
    case te::modimport::Kind::PatchedIso: return QStringLiteral("a patched disc image");
    case te::modimport::Kind::Xdelta: return QStringLiteral("an xdelta patch");
    case te::modimport::Kind::Ppf: return QStringLiteral("a PPF patch");
    case te::modimport::Kind::Bps: return QStringLiteral("a BPS patch");
    case te::modimport::Kind::Folder: return QStringLiteral("a folder of files");
    case te::modimport::Kind::Unknown: break;
    }
    return QString();
}
}  // namespace

ModImporter::ModImporter(QObject* parent) : QObject(parent), watcher_(new QFutureWatcher<ImportOutcome>(this)) {
    connect(watcher_, &QFutureWatcher<ImportOutcome>::finished, this, [this] {
        const ImportOutcome outcome = watcher_->result();
        artefact_.clear();
        emit finished(outcome);
    });
}

ModImporter::~ModImporter() {
    if (cancel_) cancel_->store(true);
    watcher_->disconnect(this);
    watcher_->waitForFinished();   // the worker reports to this object: it must not outlive it
}

bool ModImporter::running() const { return watcher_->isRunning(); }

std::string ModImporter::modIdFor(const QString& artefact) { return te::modimport::ModIdFor(localPath(artefact), nullptr); }

void ModImporter::cancel() {
    if (cancel_) cancel_->store(true);
}

bool ModImporter::start(const QString& artefact, const QString& baseIso, const QString& modsDir) {
    if (running()) return false;
    artefact_ = artefact;
    cancel_ = std::make_shared<std::atomic_bool>(false);
    auto cancelled = cancel_;
    watcher_->setFuture(QtConcurrent::run([this, artefact, baseIso, modsDir, cancelled]() {
        te::modimport::Progress p;
        p.report = [this](const std::string& stage, double fraction) {
            const QString s = QString::fromStdString(stage);
            QMetaObject::invokeMethod(this, [this, s, fraction] { emit progress(s, fraction); }, Qt::QueuedConnection);
        };
        p.cancelled = [cancelled] { return cancelled->load(); };
        std::string why;
        const te::modimport::Kind k = te::modimport::Detect(localPath(artefact), &why);
        const QString kind = k == te::modimport::Kind::Folder && QFileInfo(artefact).isFile() ? QStringLiteral("a single file") : kindLabel(k);
        QMetaObject::invokeMethod(this, [this, kind] { emit detected(kind); }, Qt::QueuedConnection);
        te::modimport::Report r;
        std::string error;
        ImportOutcome o;
        o.artefact = artefact;
        o.ok = te::modimport::Import(localPath(artefact), localPath(baseIso), localPath(modsDir), p, &r, &error);
        o.cancelled = !o.ok && cancelled->load();
        o.id = QString::fromStdString(r.id);
        o.name = QString::fromStdString(r.name);
        o.error = QString::fromStdString(error);
        o.replaced = o.ok && r.replaced;
        o.warnings = strings(r.warnings);
        o.codeChanges = strings(r.code_changes);
        o.filesChanged = strings(r.files_changed);
        o.filesAdded = strings(r.files_added);
        return o;
    }));
    return true;
}

}  // namespace ps2r::launcher
