// app/launcher/mods_page.h -- the Mods page: the built-in mods (title.json)
// and the mods folder (<mods>/<id>/mod.json, schema 1 or 2) in the overlay order, each with its switch (mods.<id>.enabled),
// region badge, summary and warning lines -- the ONLY place a folder mod is switched (the game's Options > MODS page lists
// the built-ins alone); the folder's mods reorder by drag or Move up / Move down (mods.order), open
// their folder and are removed after a confirmation. A drop, Add a mod… or Add a folder… copies a mod folder in or hands
// anything else to the import library on a worker thread (progress, Cancel, the outcome as a notice). Read when shown;
// ps2r.ini is written through the model one key at a time.
#pragma once
#include <QVector>
#include <QWidget>
#include <functional>
#include "mod_list.h"
class QLabel;
class QVBoxLayout;
namespace ps2r::launcher {
class EmptyState;
class FlatButton;
class LauncherApp;
class ModImporter;
class ModRow;
class ProgressPanel;
class SheetOverlay;
struct ImportOutcome;

class ModsPage : public QWidget {
    Q_OBJECT
public:
    explicit ModsPage(LauncherApp* app, QWidget* parent = nullptr);
    void reload();
    const QVector<ModRow*>& rows() const { return rows_; }
    ModRow* row(const QString& id) const;
    QStringList folderOrder() const;   // the folder's mods as listed (the overlay order)
    EmptyState* emptyState() const { return empty_; }
    QString noticeText() const;
    bool noticeIsError() const { return noticeError_; }
    FlatButton* addButton() const { return add_; }
    FlatButton* addFolderButton() const { return addFolder_; }
    FlatButton* cancelButton() const { return cancel_; }
    ProgressPanel* progress() const { return progress_; }
    ModImporter* importer() const { return importer_; }
    bool importing() const;

    // What a drop or a picker does with a path (the tests call it as a drop would): a mod folder is copied in, anything else
    // goes to the import library. ONE RULE when the mods folder has a mod of the id it would take: the page asks
    // first (openReplaceSheet) and the new copy replaces the old one -- its switch and its place in the order stay.
    void dropPath(const QString& path);
    // "Replace <name> <version>?" -- "The copy you have is removed first." (Cancel / Replace: objectName mods_replace_cancel /
    // mods_replace_confirm, the sentence mods_replace_text); Replace runs `replace`, Cancel adds nothing
    SheetOverlay* openReplaceSheet(const QString& id, const std::function<void()>& replace);
    SheetOverlay* replaceSheet() const { return replaceSheet_; }
    // The folder's mod `id` moved by `delta` places / to `index` (the drag's drop): writes mods.order. False when it did not move.
    bool moveMod(const QString& id, int delta);
    bool moveModTo(const QString& id, int index);
    // Remove…: the confirmation sheet (Cancel / Remove: objectName mods_remove_cancel / mods_remove_confirm); the removal
    SheetOverlay* openRemoveSheet(const QString& id);
    SheetOverlay* removeSheet() const { return removeSheet_; }
    bool removeMod(const QString& id, QString* error);
    // The importing state held for its picture (an import against a real disc is minutes; the documentation shows it)
    void showImportingForScreenshot(const QString& file, const QString& stage, double fraction);
    // The PS Vita panel at the foot: how mods reach the console, with the button right there -- Make the PS Vita
    // folder again (a PS Vita install exists) or Get the game for PS Vita… (none yet)
    QWidget* vitaPanel() const { return vitaPanel_; }
    FlatButton* vitaButton() const { return vitaButton_; }
    QString vitaPanelText() const;
    QString vitaLineText() const;
    bool makeVitaFolder();   // the button's action when there is a PS Vita install

signals:
    void setupRequested();
    void vitaSetupRequested();   // Get the game for PS Vita…: the setup for another place, PS Vita chosen

protected:
    void showEvent(QShowEvent* event) override;
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragMoveEvent(QDragMoveEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;

private:
    ModRow* addRow(const QString& id, const QString& name, QWidget* host, QVBoxLayout* layout);
    void clearFolderRows();
    void notice(const QString& text, bool error);
    void requestImport(const QString& path);
    void startImport(const QString& path);
    void importFinished(const ImportOutcome& outcome);
    // A dropped mod folder's id after the game's checks (false: *message says why it cannot be added)
    bool readyFolderId(const QString& path, QString* id, QString* name, QString* message);
    bool copyModFolder(const QString& path, bool replace, QString* message);
    void startDrag(const QString& id);
    int dropIndexAt(const QPoint& pagePos) const;
    void showDropLine(int index);
    void setBusy(bool busy);
    LauncherApp* app_;
    QWidget* builtins_;
    QVBoxLayout* builtinsLayout_;
    QWidget* folderGroup_;
    QLabel* orderNote_;
    QLabel* where_;
    QWidget* folderHost_;
    QVBoxLayout* folderLayout_;
    QWidget* dropLine_;
    QLabel* notice_;
    QWidget* importBox_;
    ProgressPanel* progress_;
    FlatButton* cancel_;
    EmptyState* empty_;
    EmptyState* notSetUp_;
    FlatButton* add_;
    FlatButton* addFolder_;
    FlatButton* openMods_;
    QWidget* vitaPanel_ = nullptr;
    QLabel* vitaText_ = nullptr;
    FlatButton* vitaButton_ = nullptr;
    QLabel* vitaLine_ = nullptr;
    ModImporter* importer_;
    SheetOverlay* removeSheet_ = nullptr;
    SheetOverlay* replaceSheet_ = nullptr;
    QVector<ModRow*> rows_;
    QVector<ModEntry> entries_;   // the folder's mods as listed
    QString importFile_, importKind_;
    bool noticeError_ = false;
    bool importBusy_ = false;   // an import started and its outcome not yet shown
    bool dragging_ = false;     // a row's drag is running (its drop is applied once the drag has ended)
    QString pendingMoveId_;
    int pendingMoveIndex_ = -1;
};

}  // namespace ps2r::launcher
