// app/launcher/setup_sheet.h -- the setup: ONE sheet over the window, four steps (Where is your game? / Where should
// the game live? / Where will you play? / Installing), the step line at the top, Back (Escape) and the step's primary
// button at the bottom. A file dropped anywhere on the sheet goes to step 1. Opened by itself on the first run; the
// Games page reopens it at step 2 for a PS Vita or another computer.
#pragma once
#include <QWidget>
class QLabel;
class QStackedWidget;
namespace ps2r::launcher {
class FlatButton;
class LauncherApp;
class SetupDiscStep;
class SetupFolderStep;
class SetupInstallStep;
class SetupTargetStep;
class SheetOverlay;

class SetupSheet : public QWidget {
    Q_OBJECT
public:
    enum Mode { FirstRun, AnotherPlace };
    SetupSheet(LauncherApp* app, QWidget* host);
    // AnotherPlace opens at step 2 with `target` chosen in step 3 (PS Vita when none is given)
    void open(Mode mode, const QString& target = QString());
    void close();
    bool isOpen() const;
    int step() const;
    void next();   // the primary button
    void back();
    FlatButton* primaryButton() const { return primary_; }
    FlatButton* backButton() const { return back_; }
    SetupDiscStep* discStep() const { return disc_; }
    SetupFolderStep* folderStep() const { return folder_; }
    SetupTargetStep* targetStep() const { return target_; }
    SetupInstallStep* installStep() const { return install_; }
    QString stepLine() const;

signals:
    void closed();
    void playRequested(const QString& isoPath);

protected:
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void go(int step);
    void refreshButtons();
    QString suggestedFolder(const QString& target) const;
    LauncherApp* app_;
    SheetOverlay* overlay_;
    QLabel* stepLine_;
    QStackedWidget* stack_;
    SetupDiscStep* disc_;
    SetupFolderStep* folder_;
    SetupTargetStep* target_;
    SetupInstallStep* install_;
    FlatButton* back_;
    FlatButton* primary_;
    Mode mode_ = FirstRun;
};

}  // namespace ps2r::launcher
