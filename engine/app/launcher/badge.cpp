// app/launcher/badge.cpp -- the badge, painted.
#include "badge.h"
#include <QFontMetrics>
#include <QPainter>
#include "launcher_theme.h"
namespace ps2r::launcher {

Badge::Badge(const QString& text, Tone tone, QWidget* parent) : QWidget(parent), text_(text), tone_(tone) {
    setFont(LauncherTheme::instance().font(LauncherTheme::Caption));
    setAccessibleName(text);
    setSizePolicy(QSizePolicy::Fixed, QSizePolicy::Fixed);
    connect(&LauncherTheme::instance(), &LauncherTheme::changed, this, [this] { update(); });
}

void Badge::setText(const QString& text) {
    text_ = text;
    setAccessibleName(text);
    updateGeometry();
    update();
}

void Badge::setTone(Tone tone) { tone_ = tone; update(); }

QSize Badge::sizeHint() const {
    const QFontMetrics fm(font());
    return QSize(fm.horizontalAdvance(text_) + 2 * LauncherTheme::S2 + 2, fm.height() + LauncherTheme::S1 + 2);
}

void Badge::paintEvent(QPaintEvent*) {
    const LauncherTheme& t = LauncherTheme::instance();
    QColor fg, bg;
    switch (tone_) {
    case Accent: fg = t.color(LauncherTheme::Accent); bg = t.color(LauncherTheme::AccentSoft); break;
    case Ok: fg = t.color(LauncherTheme::Ok); bg = fg; bg.setAlphaF(0.14f); break;
    case Warn: fg = t.color(LauncherTheme::Warn); bg = fg; bg.setAlphaF(0.16f); break;
    case Error: fg = t.color(LauncherTheme::Error); bg = fg; bg.setAlphaF(0.14f); break;
    case Neutral:
    default: fg = t.color(LauncherTheme::TextMuted); bg = t.color(LauncherTheme::SurfaceRaised); break;
    }
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QRectF r = QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5);
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(r, r.height() / 2, r.height() / 2);
    p.setPen(fg);
    p.setFont(font());
    p.drawText(rect(), Qt::AlignCenter, text_);
}

}  // namespace ps2r::launcher
