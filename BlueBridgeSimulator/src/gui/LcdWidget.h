#pragma once

#include <QImage>
#include <QWidget>

#include <cstdint>
#include <vector>

// ============================================================================
// LCD panel view: paints the CT117E-M4 LCD module framebuffer.
//
// The widget only converts pixels (RGB565 -> RGB888), fits them into its own
// area (aspect preserved, nearest neighbour so nothing gets blurred) and
// never draws text: it knows nothing about the firmware. All pixels come from
// the board's LcdController GRAM, i.e. from real ARM instructions driving the
// GPIO bus.
// ============================================================================
class LcdWidget : public QWidget {
    Q_OBJECT
public:
    // panel geometry (module mounted landscape on the CT117E-M4)
    static constexpr int kWidth = 320;
    static constexpr int kHeight = 240;

    explicit LcdWidget(QWidget* parent = nullptr);

    // RGB565, panel order (row major, 320x240)
    void setFramebuffer(const std::vector<uint16_t>& rgb565);

    QSize sizeHint() const override { return QSize(kWidth, kHeight); }
    QSize minimumSizeHint() const override { return QSize(160, 120); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QImage image_;  // Format_RGB888, 320x240
};