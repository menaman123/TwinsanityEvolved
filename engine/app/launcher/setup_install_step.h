// app/launcher/setup_install_step.h -- step 4 of the setup, "Installing": the release lookup, the download with its
// size, rate and time left, the check, the placing, then the done screen (Play for this computer; the PS Vita note or
// the carry-over note otherwise) or the error with Try again.
#pragma once
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class FlatButton;
class LauncherApp;
class ProgressPanel;

class SetupInstallStep : public QWidget {
    Q_OBJECT
public:
    SetupInstallStep(LauncherApp* app, QWidget* parent = nullptr);
    void start(const QString& target, const QString& folder);
    void retry();
    bool done() const { return done_; }
    bool failed() const { return failed_; }
    bool running() const;
    QString target() const { return target_; }
    QString folder() const { return folder_; }
    QString statusText() const;
    QString errorText() const;
    ProgressPanel* progress() const { return progress_; }
    FlatButton* showFolderButton() const { return show_; }
    // the documentation's screenshots: a download in progress / failed, held without a network
    void showProgressForScreenshot(qint64 received, qint64 total, double rate);

signals:
    void stateChanged();

private:
    void onPhase(int phase);
    void onFinished(bool ok);
    LauncherApp* app_;
    QLabel* title_;
    QLabel* line_;
    ProgressPanel* progress_;
    QLabel* error_;
    QLabel* note_;
    FlatButton* show_;
    QString target_, folder_;
    bool done_ = false, failed_ = false;
};

}  // namespace ps2r::launcher
