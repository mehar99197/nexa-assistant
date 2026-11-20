#pragma once

#include <QColor>
#include <QList>
#include <QRectF>
#include <QWidget>

class QPainter;

namespace nexa {

// A small chart of recent activity: bars (an agent's work per second, the microphone's level) or a line. Painted with
// whatever colours the caller has from the theme.
class Sparkline {
public:
    // `samples` oldest first; the tallest fills the box (a floor of `minimumPeak` keeps quiet ones low).
    static void paint(QPainter &painter, const QRectF &box, const QList<int> &samples, const QColor &colour,
                      bool bars = true, int minimumPeak = 1);
};

// A Sparkline as a widget: the core card's waveform. Its colour comes from theme.qss.
class SparklineWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
public:
    explicit SparklineWidget(QWidget *parent = nullptr);

    void setSamples(const QList<int> &samples);
    void push(int sample, int keep = 48);   // the newest sample, keeping the last `keep`
    QColor lineColor() const { return colour_; }
    void setLineColor(const QColor &colour) {
        colour_ = colour;
        update();
    }
    QSize sizeHint() const override { return {240, 28}; }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    QList<int> samples_;
    QColor colour_{0xA8, 0x55, 0xF7};
};

} // namespace nexa
