// app/launcher/badge.h -- a small rounded label: a disc's identity (PAL · original disc, PAL · modified disc,
// PAL · checking…), Not playable in this version, or a version. Painted in the theme's colours; the tone picks them.
#pragma once
#include <QWidget>
namespace ps2r::launcher {

class Badge : public QWidget {
    Q_OBJECT
public:
    enum Tone { Neutral, Accent, Ok, Warn, Error };
    explicit Badge(const QString& text, Tone tone = Neutral, QWidget* parent = nullptr);
    void setText(const QString& text);
    QString text() const { return text_; }
    void setTone(Tone tone);
    Tone tone() const { return tone_; }
    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return sizeHint(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QString text_;
    Tone tone_;
};

}  // namespace ps2r::launcher
