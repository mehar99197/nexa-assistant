#pragma once

#include "ui/NexaChrome.h"

#include <QColor>
#include <QFrame>

namespace nexa {

// A panel that wears the theme's chrome (Strategy: NexaChrome paints it). In the Flat chrome the stylesheet
// draws the panel as it always has; in any other the sheet leaves the panel bare and this frame paints its own
// fill and frame — brackets, neon, cut corners, glass, a rail — in the colours the sheet sets through
// `nexa--ChromeFrame { qproperty-… }`. Mission Control's panels and the Dashboard's tiles are ChromeFrames.
class ChromeFrame : public QFrame {
    Q_OBJECT
    Q_PROPERTY(int panelChrome READ panelChromeIndex WRITE setPanelChromeIndex)
    Q_PROPERTY(int cornerRadius READ cornerRadius WRITE setCornerRadius)
    Q_PROPERTY(QColor groundColor READ groundColor WRITE setGroundColor)
    Q_PROPERTY(QColor panelColor READ panelColor WRITE setPanelColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
    Q_PROPERTY(QColor textColor READ textColor WRITE setTextColor)
public:
    explicit ChromeFrame(QWidget *parent = nullptr);

    PanelChrome panelChrome() const { return chrome_; }
    void setPanelChrome(PanelChrome chrome);
    int panelChromeIndex() const { return static_cast<int>(chrome_); }
    void setPanelChromeIndex(int index);
    int cornerRadius() const { return radius_; }
    void setCornerRadius(int radius);
    QColor groundColor() const { return ground_; }
    QColor panelColor() const { return panel_; }
    QColor lineColor() const { return line_; }
    QColor accentColor() const { return accent_; }
    QColor textColor() const { return text_; }
    void setGroundColor(const QColor &colour) { setColour(ground_, colour); }
    void setPanelColor(const QColor &colour) { setColour(panel_, colour); }
    void setLineColor(const QColor &colour) { setColour(line_, colour); }
    void setAccentColor(const QColor &colour) { setColour(accent_, colour); }
    void setTextColor(const QColor &colour) { setColour(text_, colour); }
    ChromeColours chromeColours() const { return ChromeColours{ground_, panel_, line_, accent_, text_}; }

signals:
    void chromeChanged();       // a colour, the chrome or the corner changed

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setColour(QColor &member, const QColor &colour);

    PanelChrome chrome_{PanelChrome::Flat};
    int radius_{10};
    QColor ground_{0x0D, 0x10, 0x14};
    QColor panel_{0x15, 0x1A, 0x21};
    QColor line_{0x2A, 0x32, 0x3D};
    QColor accent_{0x46, 0xB7, 0xB0};
    QColor text_{0xE7, 0xEC, 0xF2};
};

} // namespace nexa
