// app/launcher/main_window.h -- the window: the sidebar at the left, the four pages (Games, Mods, Settings, About) in a
// stack, the sheets (the setup, Details) over the content. The setup opens itself until something is installed; a
// screenshot action (Cmd/Ctrl+Shift+S) writes the window to Logs/ for the documentation. Remembers its size and page.
#pragma once
#include <QMainWindow>
class QStackedWidget;
namespace ps2r::launcher {
class AboutPage;
class DetailsSheet;
class GamesPage;
class LauncherApp;
class ModsPage;
class SettingsPage;
class SetupSheet;
class Sidebar;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum Page { GamesIndex = 0, ModsIndex = 1, SettingsIndex = 2, AboutIndex = 3 };
    explicit MainWindow(LauncherApp* app, QWidget* parent = nullptr);
    ~MainWindow() override;

    GamesPage* gamesPage() const { return games_; }
    ModsPage* modsPage() const { return mods_; }
    SettingsPage* settingsPage() const { return settings_; }
    AboutPage* aboutPage() const { return about_; }
    SetupSheet* setupSheet() const { return setup_; }
    DetailsSheet* detailsSheet() const { return details_; }
    Sidebar* sidebar() const { return sidebar_; }
    void showPage(int index);
    int currentPage() const;
    // Writes a PNG of the window; returns its path.
    QString screenshot(const QString& path = QString());
    // Looks for a newer launcher; opens the setup when nothing is installed yet (true), else looks for the game's updates (false).
    bool startSetupIfNeeded();
    void openSetupForAnotherPlace();

protected:
    void closeEvent(QCloseEvent* event) override;

private:
    void refreshFooter();
    LauncherApp* app_;
    Sidebar* sidebar_;
    QStackedWidget* stack_;
    QWidget* content_;
    GamesPage* games_;
    ModsPage* mods_;
    SettingsPage* settings_;
    AboutPage* about_;
    DetailsSheet* details_;
    SetupSheet* setup_;
};

}  // namespace ps2r::launcher
