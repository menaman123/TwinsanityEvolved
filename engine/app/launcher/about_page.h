// app/launcher/about_page.h -- About: what this is, the versions (the launcher, Qt, the game installed and for which
// machine), the launcher's own update (LauncherUpdateBox: Check for a newer launcher / Update the launcher to <tag>), the
// places (the game folder, the settings file, the launcher's own data, the logs -- each with Open), the PS Vita in six
// sentences (title.json targets.vita.about), the licences, the project page (the source, for building it
// yourself) and Remove… (the launcher's data; on request the game folder too).
#pragma once
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace ps2r::launcher {
class DetailsSheet;
class FlatButton;
class LauncherApp;
class LauncherUpdateBox;

class AboutPage : public QWidget {
    Q_OBJECT
public:
    AboutPage(LauncherApp* app, DetailsSheet* details, QWidget* parent = nullptr);
    FlatButton* removeButton() const { return remove_; }
    void refresh();
    QString versionsText() const;
    QString vitaText() const;   // the PS Vita section's sentences (plain text, one a line)
    LauncherUpdateBox* launcherUpdateBox() const { return launcherUpdate_; }

protected:
    void showEvent(QShowEvent* event) override;
    bool eventFilter(QObject* watched, QEvent* event) override;

private:
    void openRemoveSheet();
    QWidget* linkRow(const QString& label, const QString& path, QWidget* parent);
    LauncherApp* app_;
    DetailsSheet* details_;
    FlatButton* remove_;
    QLabel* versions_;
    LauncherUpdateBox* launcherUpdate_;
    QLabel* vita_ = nullptr;
    QWidget* places_;
    QVBoxLayout* placesLayout_;
};

}  // namespace ps2r::launcher
