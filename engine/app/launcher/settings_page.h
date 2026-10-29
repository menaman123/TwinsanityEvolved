// app/launcher/settings_page.h -- Settings: the manifest's groups and rows mirroring the game folder's ps2r.ini in plain
// language -- choices and switches (SettingsRow), the folders (paths.memcards / paths.mods: relative to the game
// folder when inside it) and the disc the game starts with (disc-image.txt). The file is read when the page is shown
// and one key is written when a row changes, through IniSettings (the only writer); no second store. The frame rate's
// Uncapped locks vertical sync, as the game's own menu does. The PS Vita group carries "Make the PS Vita folder again"
// (the PS Vita install's folder made from the game folder once more: a newly imported mod, a changed setting).
#pragma once
#include <QHash>
#include <QVector>
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class EmptyState;
class FlatButton;
class FolderRow;
class LauncherApp;
class SettingsRow;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(LauncherApp* app, QWidget* parent = nullptr);
    void reload();   // reads every row from the options file
    SettingsRow* row(const QString& key) const;
    const QVector<SettingsRow*>& rows() const { return rows_; }
    FolderRow* folderRow(const QString& key) const { return folders_.value(key); }
    QStringList folderKeys() const { return folders_.keys(); }
    EmptyState* emptyState() const { return empty_; }
    QString errorText() const;
    // the folder rows' and the disc row's Change…, with the choice already made (the tests and the dialogs share them)
    bool chooseFolder(const QString& key, const QString& dir);
    void chooseDisc(const QString& iso);
    // the PS Vita group's "Make the PS Vita folder again" (shown while there is a PS Vita install) and its line
    FlatButton* vitaFolderButton() const { return vitaButton_; }
    QString vitaFolderLine() const;
    bool makeVitaFolder();   // the button's action

signals:
    void setupRequested();

protected:
    void showEvent(QShowEvent* event) override;

private:
    void applyDependencies();
    void showError(const QString& text);
    LauncherApp* app_;
    QWidget* body_;
    EmptyState* empty_;
    QLabel* error_;
    QLabel* note_;
    FlatButton* vitaButton_ = nullptr;
    QLabel* vitaLine_ = nullptr;
    QVector<SettingsRow*> rows_;
    QHash<QString, FolderRow*> folders_;   // paths.* and the disc row (key: the disc file's name)
    QString discKey_;
    QString pendingDisc_;
};

}  // namespace ps2r::launcher
