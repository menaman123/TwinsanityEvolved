// app/launcher/setup_folder_step.h -- step 2 of the setup, "Where should the game live?": the ONE game folder (the
// suggestion, or one the user picks), what will be in it, the space it needs and has, and why a folder cannot be used.
#pragma once
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class FolderRow;
class LauncherApp;

class SetupFolderStep : public QWidget {
    Q_OBJECT
public:
    SetupFolderStep(LauncherApp* app, QWidget* parent = nullptr);
    void setFolder(const QString& folder, bool byUser = false);
    QString folder() const { return folder_; }
    bool chosenByUser() const { return byUser_; }
    bool valid() const { return valid_; }
    QString problem() const;   // the error line, or ""
    QString notice() const;    // "The game is already here…", or ""
    FolderRow* folderRow() const { return row_; }

signals:
    void validityChanged(bool valid);

private:
    void validate();
    void pick();
    LauncherApp* app_;
    FolderRow* row_;
    QLabel* contents_;
    QLabel* space_;
    QLabel* problem_;
    QLabel* notice_;
    QString folder_;
    bool valid_ = false, byUser_ = false;
};

}  // namespace ps2r::launcher
