#pragma once

#include "ui/NexaCoreSpec.h"

#include <QColor>
#include <QPointF>
#include <QRectF>

#include <span>

class QPainter;

namespace nexa {

// The colours one frame is painted with: the state's colour plus the theme's neutrals.
struct CorePaint {
    QColor accent;      // the state's colour (mixed while two states cross-fade)
    QColor act;         // packets and lit nodes
    QColor wake;        // the privacy ring while the wake word is on
    QColor panel;
    QColor line;
    QColor muted;
    QColor text;
    QColor ground;
    QColor glyph;       // the N on the core
};

struct CoreGeometry {
    bool nodes{true};       // the four satellites on the dotted ring
    bool labels{false};     // their names (the presence has room for them, the button does not)
};

// One state's moving parts at some opacity; two of them overlap during a cross-fade.
struct CoreLayer {
    CoreMode mode{CoreMode::Idle};
    CoreFrame frame;
    QColor colour;          // that state's own colour
    double opacity{1.0};
};

// Paints the core (Strategy for the drawing alone, so the widget, the tray icon and a still picture
// all draw the very same thing). About 60 primitives a frame, no blur and no QGraphicsEffect: the
// halo is two translucent circles. Drawn in this order: halo, dotted ring, each layer's motion
// (ticks, scan arc, satellites, sonar, comet), the nodes, the core, and the packets on top.
class NexaCorePainter {
public:
    static void paint(QPainter &p, const QRectF &rect, const CorePaint &paint, const CoreGeometry &geometry,
                      CoreNodes lit, double coreScale, std::span<const CoreLayer> layers);

    // A node's icon in a box of `size` around `centre`; also used for buttons that mean the same.
    static void paintNodeIcon(QPainter &p, CoreNode node, const QPointF &centre, double size, const QColor &colour);
};

} // namespace nexa
