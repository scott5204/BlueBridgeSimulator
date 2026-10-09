#include "gui/LedIndicator.h"

#include <QPainter>

LedIndicator::LedIndicator(const QString& label, QWidget* parent)
    : QWidget(parent), label_(label) {
    setMinimumSize(36, 48);
}

QSize LedIndicator::sizeHint() const { return QSize(36, 48); }

void LedIndicator::setOn(bool on) {
    if (on_ == on) return;
    on_ = on;
    update();
}

void LedIndicator::paintEvent(QPaintEvent*) {
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    const int w = width(), h = height();
    const int d = qMin(w - 8, h - 20);
    const QRect circle((w - d) / 2, 4, d, d);

    if (on_) {
        QRadialGradient g(circle.center(), d / 2.0);
        g.setColorAt(0.0, QColor(255, 120, 120));
        g.setColorAt(0.6, QColor(255, 0, 0));
        g.setColorAt(1.0, QColor(160, 0, 0));
        p.setBrush(QBrush(g));
        p.setPen(QPen(QColor(120, 0, 0), 1));
    } else {
        p.setBrush(QColor(70, 10, 10));
        p.setPen(QPen(QColor(50, 20, 20), 1));
    }
    p.drawEllipse(circle);

    p.setPen(QPen(palette().color(QPalette::Text)));
    QFont f = p.font();
    f.setPointSize(8);
    p.setFont(f);
    p.drawText(QRect(0, d + 6, w, 16), Qt::AlignCenter, label_);
}
