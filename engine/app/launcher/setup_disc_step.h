// app/launcher/setup_disc_step.h -- step 1 of the setup, "Where is your game?": the search of the usual folders, every
// candidate identified by content and shown as a card (several may be chosen; each becomes a game card), a file chosen
// or dropped, and the step's three states -- looking, found, empty -- plus the refusal banner for a file that is not
// the game.
#pragma once
#include <QVector>
#include <QWidget>
#include "disc_scanner.h"
class QLabel;
class QScrollArea;
class QVBoxLayout;
namespace ps2r::launcher {
class DiscChoiceCard;
class EmptyState;
class FlatButton;
class LauncherApp;
class ProgressPanel;

class SetupDiscStep : public QWidget {
    Q_OBJECT
public:
    SetupDiscStep(LauncherApp* app, QWidget* parent = nullptr);
    // Starts the search the first time the step is shown (and again on Search again).
    void begin();
    void search();
    void addFromPath(const QString& path);   // a chosen or dropped file
    QVector<DiscFound> chosen() const;
    bool hasChoice() const;
    const QVector<DiscChoiceCard*>& cards() const { return cards_; }
    bool looking() const;
    QString bannerText() const;
    EmptyState* emptyState() const { return empty_; }
    FlatButton* chooseButton() const { return choose_; }
    FlatButton* searchButton() const { return again_; }
    void showLookingForScreenshot();   // the looking state held (the documentation's screenshot)

signals:
    void choiceChanged();

private:
    void addCard(const DiscFound& disc, bool choose);
    void refresh();
    void choose();
    LauncherApp* app_;
    QLabel* banner_;
    ProgressPanel* progress_;
    QScrollArea* scroll_;
    QWidget* cardsHost_;
    QVBoxLayout* cardsLayout_;
    EmptyState* empty_;
    FlatButton* choose_;
    FlatButton* again_;
    QVector<DiscChoiceCard*> cards_;
    QString wantChosen_;   // a file the user chose or dropped: its card is chosen when it arrives
    bool begun_ = false, searching_ = false, checking_ = false;
};

}  // namespace ps2r::launcher
