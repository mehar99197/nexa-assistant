#include "ui/ChromeFrame.h"

#include <QPainter>

namespace nexa {

ChromeFrame::ChromeFrame(QWidget *parent) : QFrame(parent) {}

void ChromeFrame::setPanelChrome(PanelChrome chrome) {
    if (chrome_ == chrome) return;
    chrome_ = chrome;
    emit chromeChanged();
    update();
}

void ChromeFrame::setPanelChromeIndex(int index) {
    if (index < 0 || index >= kPanelChromeCount) return;
    setPanelChrome(kPanelChromes.at(static_cast<size_t>(index)));
}

void ChromeFrame::setCornerRadius(int radius) {
    if (radius_ == radius) return;
    radius_ = radius;
    emit chromeChanged();
    update();
}

void ChromeFrame::setColour(QColor &member, const QColor &colour) {
    member = colour;
    emit chromeChanged();
    update();
}

void ChromeFrame::paintEvent(QPaintEvent *event) {
    if (chrome_ != PanelChrome::Flat) {
        QPainter painter(this);
        paintPanelChrome(painter, QRectF(rect()), chrome_, radius_, chromeColours());
    }
    QFrame::paintEvent(event);      // the sheet's own panel in the Flat chrome; nothing otherwise
}

} // namespace nexa
