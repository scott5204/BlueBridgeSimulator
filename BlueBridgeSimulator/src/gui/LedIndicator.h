#pragma once

#include <QWidget>

// Round LED indicator widget for the virtual board panel.
class LedIndicator : public QWidget {
    Q_OBJECT
public:
    explicit LedIndicator(const QString& label, QWidget* parent = nullptr);

    void setOn(bool on);
    bool isOn() const { return on_; }

protected:
    void paintEvent(QPaintEvent* event) override;
    QSize sizeHint() const override;

private:
    QString label_;
    bool on_ = false;
};
