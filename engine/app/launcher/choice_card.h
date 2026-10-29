// app/launcher/choice_card.h -- one of a set of choices drawn as a card: a title, a line, an optional detail and an
// optional control below (the platforms of "Another computer"); the chosen card has the accent border. A click or Space
// chooses it.
#pragma once
#include <QWidget>
class QLabel;
class QVBoxLayout;
namespace ps2r::launcher {

class ChoiceCard : public QWidget {
    Q_OBJECT
public:
    ChoiceCard(const QString& title, const QString& line, QWidget* parent = nullptr);
    void setDetail(const QString& detail);
    void setExtra(QWidget* extra);   // shown below the line while the card is chosen
    void setChosen(bool chosen);
    bool chosen() const { return chosen_; }
    QString title() const;

signals:
    void chosenByUser();

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;
    void enterEvent(QEnterEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    void restyle();
    QLabel* title_;
    QLabel* line_;
    QLabel* detail_;
    QWidget* extra_ = nullptr;
    QVBoxLayout* col_;
    bool chosen_ = false, hover_ = false;
};

}  // namespace ps2r::launcher
