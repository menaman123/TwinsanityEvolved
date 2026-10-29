// app/launcher/launcher_update_box.h -- the launcher's own update as one box: on the Games page (the page the
// launcher opens on) while there is something to say, and on About always. Its line is SelfUpdate's sentence (or, after
// an update, "The launcher was updated to <tag>."), a progress bar while the new launcher downloads, and the buttons the
// state allows: Update the launcher to <tag> and Not now (offered), Download it (it cannot replace itself where it is,
// or an update failed: the release's page in the browser), Try again (failed), Check for a newer launcher (About).
#pragma once
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class FlatButton;
class LauncherApp;
class ProgressPanel;

class LauncherUpdateBox : public QWidget {
    Q_OBJECT
public:
    enum Place { OnGames, OnAbout };
    LauncherUpdateBox(LauncherApp* app, Place place, QWidget* parent = nullptr);
    void refresh();
    QString text() const;
    FlatButton* updateButton() const { return update_; }
    FlatButton* laterButton() const { return later_; }
    FlatButton* downloadButton() const { return download_; }
    FlatButton* retryButton() const { return retry_; }
    FlatButton* checkButton() const { return check_; }
    ProgressPanel* progress() const { return progress_; }

private:
    void restyle();
    LauncherApp* app_;
    Place place_;
    QLabel* line_;
    ProgressPanel* progress_;
    FlatButton* update_;
    FlatButton* later_;
    FlatButton* download_;
    FlatButton* retry_;
    FlatButton* check_;
    bool finishedSeen_ = false;   // Games: the after-update line closed with Not now
};

}  // namespace ps2r::launcher
