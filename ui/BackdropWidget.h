#pragma once

#include "ui/NexaChrome.h"

#include <QColor>
#include <QWidget>

namespace nexa {

// The ground the window's panels lie on, wearing the theme's backdrop (Strategy: NexaChrome paints it): the
// bare ground for most themes, a grid, dots, a glow, a horizon or scan lines for the HUD ones. The main window's
// central widget; what it holds is transparent where the backdrop should show.
class BackdropWidget : public QWidget {
    Q_OBJECT
    Q_PROPERTY(int backdrop READ backdropIndex WRITE setBackdropIndex)
    Q_PROPERTY(QColor groundColor READ groundColor WRITE setGroundColor)
    Q_PROPERTY(QColor lineColor READ lineColor WRITE setLineColor)
    Q_PROPERTY(QColor accentColor READ accentColor WRITE setAccentColor)
public:
    explicit BackdropWidget(QWidget *parent = nullptr);

    Backdrop backdrop() const { return backdrop_; }
    void setBackdrop(Backdrop backdrop);
    int backdropIndex() const { return static_cast<int>(backdrop_); }
    void setBackdropIndex(int index);
    QColor groundColor() const { return ground_; }
    QColor lineColor() const { return line_; }
    QColor accentColor() const { return accent_; }
    void setGroundColor(const QColor &colour) { setColour(ground_, colour); }
    void setLineColor(const QColor &colour) { setColour(line_, colour); }
    void setAccentColor(const QColor &colour) { setColour(accent_, colour); }

protected:
    void paintEvent(QPaintEvent *event) override;

private:
    void setColour(QColor &member, const QColor &colour);

    Backdrop backdrop_{Backdrop::Plain};
    QColor ground_{0x0D, 0x10, 0x14};
    QColor line_{0x2A, 0x32, 0x3D};
    QColor accent_{0x46, 0xB7, 0xB0};
};

} // namespace nexa
