// app/launcher/game_card.h -- a chosen disc's card on Games: the generated artwork, the title, the region and state
// badges, the details line, the status line, the facts (last played, playtime, the version installed) and the actions
// (Play; Update to <tag> when a newer release is out; Locate… and Remove when the image moved).
#pragma once
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class Badge;
class FlatButton;
class LauncherApp;

class GameCard : public QWidget {
    Q_OBJECT
public:
    GameCard(LauncherApp* app, const QString& isoPath, QWidget* parent = nullptr);
    QString isoPath() const { return iso_; }
    void refresh();
    FlatButton* actionButton() const { return action_; }
    FlatButton* updateButton() const { return update_; }
    FlatButton* removeButton() const { return remove_; }
    QString statusText() const;
    QString factsText() const;
    QString stateBadgeText() const;      // "Not playable in this version", else ""
    QString identityBadgeText() const;   // "PAL · modified disc"

signals:
    void playRequested();
    void installRequested();
    void updateRequested();
    void locateRequested();
    void removeRequested();

protected:
    void paintEvent(QPaintEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;

private:
    void restyle();
    LauncherApp* app_;
    QString iso_;
    QLabel* title_;
    Badge* region_;
    Badge* state_;
    QLabel* details_;
    QLabel* status_;
    QLabel* facts_;
    FlatButton* action_;
    FlatButton* update_;
    FlatButton* remove_;
};

}  // namespace ps2r::launcher
