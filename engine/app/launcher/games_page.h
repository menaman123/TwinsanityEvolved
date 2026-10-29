// app/launcher/games_page.h -- Games: the launcher's own update when there is one (LauncherUpdateBox, at the top: the
// page the launcher opens on), one card per chosen disc (Play / Update / Locate…), Add a disc image… and Search again,
// the page as a drop zone, the empty and error states, the installs (this computer, PS Vita, another computer: each with
// Open and its update), an update's progress, and "Get the game for PS Vita or another computer…".
#pragma once
#include <QVector>
#include <QWidget>
class QLabel;
class QScrollArea;
class QVBoxLayout;
namespace ps2r::launcher {
class EmptyState;
class FlatButton;
class GameCard;
class LauncherApp;
class LauncherUpdateBox;
class ProgressPanel;

class GamesPage : public QWidget {
    Q_OBJECT
public:
    explicit GamesPage(LauncherApp* app, QWidget* parent = nullptr);
    void rebuildCards();
    void rebuildInstalls();
    GameCard* card(const QString& isoPath) const;
    const QVector<GameCard*>& cards() const { return cards_; }
    EmptyState* emptyState() const { return empty_; }
    FlatButton* addButton() const { return add_; }
    FlatButton* searchButton() const { return search_; }
    FlatButton* anotherPlaceButton() const { return another_; }
    QString bannerText() const;
    LauncherUpdateBox* launcherUpdateBox() const { return launcherUpdate_; }
    QString updateText() const;
    void play(const QString& isoPath);   // the card's Play, as a method the test drivers share
    void addDiscFromPath(const QString& path);
    void showRefusal(const QString& path, const QString& reason);

signals:
    void anotherPlaceRequested();
    void installHereRequested();

protected:
    void dragEnterEvent(QDragEnterEvent* event) override;
    void dragLeaveEvent(QDragLeaveEvent* event) override;
    void dropEvent(QDropEvent* event) override;
    void paintEvent(QPaintEvent* event) override;
    void showEvent(QShowEvent* event) override;

private:
    void chooseDisc();
    void locate(const QString& isoPath);
    void refreshCards();
    void startUpdate(const QString& target);
    LauncherApp* app_;
    QScrollArea* scroll_;
    LauncherUpdateBox* launcherUpdate_;
    QLabel* banner_;
    QWidget* cardsHost_;
    QVBoxLayout* cardsLayout_;
    EmptyState* empty_;
    QVector<GameCard*> cards_;
    FlatButton* add_;
    FlatButton* search_;
    QLabel* searchNote_;
    QWidget* installsHost_;
    QVBoxLayout* installsLayout_;
    ProgressPanel* updateProgress_;
    QLabel* updateLine_;
    FlatButton* another_;
    QString locating_;      // the card whose disc the user is locating
    QString updating_;      // the target being updated from this page
    bool dragOver_ = false;
};

}  // namespace ps2r::launcher
