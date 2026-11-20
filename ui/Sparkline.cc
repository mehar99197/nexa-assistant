#include "ui/Sparkline.h"

#include <QPainter>
#include <QPainterPath>

#include <algorithm>

namespace nexa {

void Sparkline::paint(QPainter &painter, const QRectF &box, const QList<int> &samples, const QColor &colour, bool bars,
                      int minimumPeak) {
    if (samples.isEmpty() || box.width() <= 0 || box.height() <= 0) return;
    const int peak = std::max(minimumPeak, *std::max_element(samples.cbegin(), samples.cend()));
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const double step = box.width() / samples.size();
    if (bars) {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colour);
        const double width = std::max(1.5, step * 0.55);
        for (qsizetype i = 0; i < samples.size(); ++i) {
            // A quiet second still shows as a stub, so the row reads as the agent's.
            const double height = std::max(2.0, box.height() * std::clamp(double(samples.at(i)) / peak, 0.0, 1.0));
            painter.drawRoundedRect(QRectF(box.left() + i * step, box.bottom() - height, width, height), 1, 1);
        }
    } else {
        QPainterPath line;
        for (qsizetype i = 0; i < samples.size(); ++i) {
            const QPointF point(box.left() + (i + 0.5) * step,
                                box.bottom() - box.height() * std::clamp(double(samples.at(i)) / peak, 0.0, 1.0));
            if (i == 0) {
                line.moveTo(point);
            } else {
                line.lineTo(point);
            }
        }
        painter.setPen(QPen(colour, 1.6));
        painter.drawPath(line);
    }
    painter.restore();
}

SparklineWidget::SparklineWidget(QWidget *parent) : QWidget(parent) {
    setMinimumHeight(18);
}

void SparklineWidget::setSamples(const QList<int> &samples) {
    samples_ = samples;
    update();
}

void SparklineWidget::push(int sample, int keep) {
    samples_.append(sample);
    if (samples_.size() > keep) samples_.remove(0, samples_.size() - keep);
    update();
}

void SparklineWidget::paintEvent(QPaintEvent *) {
    QPainter painter(this);
    Sparkline::paint(painter, QRectF(rect()).adjusted(1, 2, -1, -2), samples_, colour_, true, 100);
}

} // namespace nexa
