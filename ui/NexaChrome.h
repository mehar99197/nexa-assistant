#pragma once

#include <QColor>
#include <QPainterPath>
#include <QRectF>
#include <QString>
#include <QStringView>

#include <array>
#include <optional>

class QPainter;

namespace nexa {

// The chrome of a theme, pure (a Strategy per style, painted by ChromeFrame and BackdropWidget): how its panels
// are framed and what lies behind them. A palette is colours; the chrome is what the HUD themes add — corner
// brackets, neon edges, cut corners, glass, a rail — and the grid or glow the window shows between the panels.
// Flat and Plain are the catalogue as it was: the stylesheet's rounded panel on the bare ground.

enum class PanelChrome {
    Flat,       // the sheet draws the panel: a rounded fill with a hairline
    Brackets,   // a square hairline frame with L-shaped brackets in the accent at the corners
    Glow,       // the frame in the accent, lit from the inside, as neon
    Chamfer,    // the corners cut, the cuts stroked in the accent
    Glass,      // a translucent pane with a light along its top edge
    Rail,       // a hairline frame with a bar of the accent down the left edge
};
inline constexpr int kPanelChromeCount = 6;
inline constexpr std::array<PanelChrome, kPanelChromeCount> kPanelChromes{
    PanelChrome::Flat, PanelChrome::Brackets, PanelChrome::Glow, PanelChrome::Chamfer, PanelChrome::Glass, PanelChrome::Rail};
QString panelChromeName(PanelChrome chrome);        // "Brackets"
QString panelChromeKey(PanelChrome chrome);         // "brackets"
QString panelChromeBlurb(PanelChrome chrome);       // one line for the theme gallery
std::optional<PanelChrome> panelChromeFromKey(QStringView key);

enum class Backdrop {
    Plain,      // the ground alone
    Grid,       // a fine grid, the accent glowing behind the middle
    Dots,       // a lattice of dots, the accent glowing behind the middle
    Vignette,   // the accent glowing behind the middle, the edges falling into shadow
    Horizon,    // a perspective grid running to a horizon low in the window
    Scan,       // scan lines across the ground, a faint glow behind the middle
};
inline constexpr int kBackdropCount = 6;
inline constexpr std::array<Backdrop, kBackdropCount> kBackdrops{
    Backdrop::Plain, Backdrop::Grid, Backdrop::Dots, Backdrop::Vignette, Backdrop::Horizon, Backdrop::Scan};
QString backdropName(Backdrop backdrop);            // "Grid"
QString backdropKey(Backdrop backdrop);             // "grid"
std::optional<Backdrop> backdropFromKey(QStringView key);

// What a theme pack carries of the two.
struct ThemeChrome {
    PanelChrome panels{PanelChrome::Flat};
    Backdrop backdrop{Backdrop::Plain};
};

// The colours the chrome is painted in: the theme's own tokens.
struct ChromeColours {
    QColor ground;
    QColor panel;
    QColor line;
    QColor accent;
    QColor text;
};

// The backdrop over `rect` (the ground is already there). Plain paints nothing.
void paintBackdrop(QPainter &painter, const QRectF &rect, Backdrop backdrop, const ChromeColours &colours);
// A panel's fill and frame within `rect`, in a chrome; `radius` is the design's corner. Flat paints nothing: the
// stylesheet draws that one.
void paintPanelChrome(QPainter &painter, const QRectF &rect, PanelChrome chrome, double radius, const ChromeColours &colours);
// The outline of a panel in a chrome: cut at the corners for Chamfer, rounded otherwise.
QPainterPath panelOutline(const QRectF &rect, PanelChrome chrome, double radius);
// How much of the design's corner a chrome keeps: brackets want a square frame, a chamfer has no round corner.
double panelRadius(PanelChrome chrome, double designRadius);

} // namespace nexa
