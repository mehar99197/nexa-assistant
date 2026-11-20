#include "ui/BackdropWidget.h"

#include <QPaintEvent>
#include <QPainter>

namespace nexa {

BackdropWidget::BackdropWidget(QWidget *parent) : QWidget(parent) {
    setAttribute(Qt::WA_OpaquePaintEvent, true);     // every pixel is painted here
    setAutoFillBackground(false);
}

void BackdropWidget::setBackdrop(Backdrop backdrop) {
    if (backdrop_ == backdrop) return;
    backdrop_ = backdrop;
    update();
}

void BackdropWidget::setBackdropIndex(int index) {
    if (index < 0 || index >= kBackdropCount) return;
    setBackdrop(kBackdrops.at(static_cast<size_t>(index)));
}

void BackdropWidget::setColour(QColor &member, const QColor &colour) {
    member = colour;
    update();
}

void BackdropWidget::paintEvent(QPaintEvent *event) {
    QPainter painter(this);
    painter.setClipRect(event->rect());
    painter.fillRect(rect(), ground_);
    paintBackdrop(painter, QRectF(rect()), backdrop_, ChromeColours{ground_, QColor(), line_, accent_, QColor()});
}

} // namespace nexa
