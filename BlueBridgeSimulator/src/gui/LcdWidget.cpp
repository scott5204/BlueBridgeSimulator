#include "gui/LcdWidget.h"

#include <QPainter>

#include <algorithm>

LcdWidget::LcdWidget(QWidget* parent) : QWidget(parent) {
    image_ = QImage(kWidth, kHeight, QImage::Format_RGB888);
    image_.fill(Qt::black);
    setMinimumSize(minimumSizeHint());
}

void LcdWidget::setFramebuffer(const std::vector<uint16_t>& rgb565) {
    if (rgb565.size() != size_t(kWidth) * kHeight) return;

    for (int y = 0; y < kHeight; y++) {
        uchar* out = image_.scanLine(y);
        const uint16_t* in = rgb565.data() + size_t(y) * kWidth;
        for (int x = 0; x < kWidth; x++) {
            const uint16_t p = in[x];
            const uint32_t r5 = (p >> 11) & 0x1Fu;
            const uint32_t g6 = (p >> 5) & 0x3Fu;
            const uint32_t b5 = p & 0x1Fu;
            // 5/6-bit -> 8-bit with bit replication (datasheet-neutral:
            // the raw GRAM value is displayed as standard RGB565)
            out[0] = uchar((r5 << 3) | (r5 >> 2));
            out[1] = uchar((g6 << 2) | (g6 >> 4));
            out[2] = uchar((b5 << 3) | (b5 >> 2));
            out += 3;
        }
    }
    update();
}

void LcdWidget::paintEvent(QPaintEvent* event) {
    (void)event;
    QPainter painter(this);
    painter.fillRect(rect(), QColor(0x16, 0x21, 0x3e));

    // Fill the widget with the panel: aspect ratio preserved, scaled to the
    // largest size that fits, sampled with nearest neighbour (no smoothing)
    // so the GRAM pixels stay sharp. Integer-only zoom left wide margins on
    // a non-multiple widget size, which made the screen look small.
    const double scale =
        std::min(double(width()) / kWidth, double(height()) / kHeight);
    const int dw = std::max(1, int(kWidth * scale));
    const int dh = std::max(1, int(kHeight * scale));
    const int dx = (width() - dw) / 2;
    const int dy = (height() - dh) / 2;

    painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
    painter.drawImage(QRect(dx, dy, dw, dh), image_);

    painter.setPen(QColor(0x0d, 0x17, 0x30));
    painter.drawRect(QRect(dx - 1, dy - 1, dw + 1, dh + 1));
}