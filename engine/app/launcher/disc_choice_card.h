// app/launcher/disc_choice_card.h -- a disc image found in step 1 of the setup: the generated artwork, the title, the
// identity badge ("PAL · modified disc"; "PAL · checking…" while the image is hashed), the playability badge when it cannot
// be played, the details line (serial · size · folder) and the verdict line, with a check mark when it is chosen. A click
// or Space toggles it. setDisc() takes the verdict when the image check finishes.
#pragma once
#include <QWidget>
#include "disc_scanner.h"
class QLabel;
namespace ps2r::launcher {
class Badge;

class DiscChoiceCard : public QWidget {
    Q_OBJECT
public:
    DiscChoiceCard(const QString& titleName, const DiscFound& disc, bool playable, const QString& notPlayable, QWidget* parent = nullptr);
    const DiscFound& disc() const { return disc_; }
    void setDisc(const DiscFound& disc);
    bool chosen() const { return chosen_; }
    bool playable() const { return playable_; }
    void setChosen(bool chosen);
    QString identityBadgeText() const;
    QString stateBadgeText() const;   // "" while playable
    QString stateLineText() const;

signals:
    void toggled(bool chosen);

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    void restyle();
    void showDisc();
    DiscFound disc_;
    bool playable_;
    QString notPlayable_;
    QLabel* title_;
    QLabel* details_;
    QLabel* state_;
    Badge* identity_;
    Badge* stateBadge_;
    bool chosen_ = false;
};

}  // namespace ps2r::launcher
