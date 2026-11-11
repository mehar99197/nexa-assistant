#include "ui/NexaChrome.h"

#include <QLineF>
#include <QList>
#include <QPainter>
#include <QPen>
#include <QPointF>
#include <QRadialGradient>
#include <QLinearGradient>

#include <algorithm>
#include <cmath>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QColor withAlpha(QColor colour, double alpha) {
    colour.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return colour;
}

// The accent glowing behind the middle of the window, a little above centre, where the core sits.
void glowBehindMiddle(QPainter &painter, const QRectF &rect, const QColor &accent, double alpha, double height = 0.42) {
    const QPointF centre(rect.center().x(), rect.top() + rect.height() * height);
    QRadialGradient gradient(centre, std::max(rect.width(), rect.height()) * 0.55);
    gradient.setColorAt(0.0, withAlpha(accent, alpha));
    gradient.setColorAt(0.55, withAlpha(accent, alpha * 0.25));
    gradient.setColorAt(1.0, withAlpha(accent, 0.0));
    painter.fillRect(rect, gradient);
}

void grid(QPainter &painter, const QRectF &rect, const QColor &line, double step, double alpha, double boldAlpha) {
    QList<QLineF> faint;
    QList<QLineF> bold;
    int n = 0;
    for (double x = rect.left() + step; x < rect.right(); x += step, ++n) {
        (n % 4 == 3 ? bold : faint).append(QLineF(x + 0.5, rect.top(), x + 0.5, rect.bottom()));
    }
    n = 0;
    for (double y = rect.top() + step; y < rect.bottom(); y += step, ++n) {
        (n % 4 == 3 ? bold : faint).append(QLineF(rect.left(), y + 0.5, rect.right(), y + 0.5));
    }
    painter.setPen(QPen(withAlpha(line, alpha), 1));
    painter.drawLines(faint);
    painter.setPen(QPen(withAlpha(line, boldAlpha), 1));
    painter.drawLines(bold);
}

void dots(QPainter &painter, const QRectF &rect, const QColor &line, double step, double alpha) {
    QList<QPointF> points;
    for (double y = rect.top() + step / 2; y < rect.bottom(); y += step) {
        for (double x = rect.left() + step / 2; x < rect.right(); x += step) points.append(QPointF(x, y));
    }
    painter.setPen(QPen(withAlpha(line, alpha), 2.2, Qt::SolidLine, Qt::RoundCap));
    painter.drawPoints(points);
}

void scanLines(QPainter &painter, const QRectF &rect, const QColor &line, double alpha) {
    QList<QLineF> lines;
    for (double y = rect.top() + 1.5; y < rect.bottom(); y += 4) lines.append(QLineF(rect.left(), y, rect.right(), y));
    painter.setPen(QPen(withAlpha(line, alpha), 1));
    painter.drawLines(lines);
}

void vignette(QPainter &painter, const QRectF &rect, double alpha) {
    const QPointF centre = rect.center();
    QRadialGradient gradient(centre, std::hypot(rect.width(), rect.height()) * 0.5);
    gradient.setColorAt(0.0, withAlpha(Qt::black, 0.0));
    gradient.setColorAt(0.6, withAlpha(Qt::black, 0.0));
    gradient.setColorAt(1.0, withAlpha(Qt::black, alpha));
    painter.fillRect(rect, gradient);
}

// A perspective grid running from a horizon low in the window to its bottom edge.
void horizon(QPainter &painter, const QRectF &rect, const QColor &accent, double alpha) {
    const double horizonY = rect.top() + rect.height() * 0.62;
    const QPointF vanishing(rect.center().x(), horizonY);
    const double depth = rect.bottom() - horizonY;
    QList<QLineF> lines;
    constexpr int kRows = 9;
    for (int i = 1; i <= kRows; ++i) {
        const double t = double(i) / kRows;
        const double y = horizonY + depth * t * t;
        lines.append(QLineF(rect.left(), y, rect.right(), y));
    }
    // The spokes start a little below the horizon, so they never pile up into one bright point.
    constexpr int kSpokes = 7;
    const double spread = rect.width() * 0.16;
    for (int k = -kSpokes; k <= kSpokes; ++k) {
        const QPointF foot(vanishing.x() + k * spread, rect.bottom());
        const QLineF spoke(vanishing, foot);
        lines.append(QLineF(spoke.pointAt(0.08), foot));
    }
    painter.setPen(QPen(withAlpha(accent, alpha), 1));
    painter.drawLines(lines);
    painter.setPen(QPen(withAlpha(accent, alpha * 1.4), 1));
    painter.drawLine(QLineF(rect.left(), horizonY, rect.right(), horizonY));
}

// The L-shaped brackets of a HUD panel: `length` along each edge from the corner, `width` thick, in the accent.
void brackets(QPainter &painter, const QRectF &box, double length, double width, const QColor &accent) {
    struct Corner {
        QPointF at;
        QPointF dx;     // along the top or bottom edge, inwards
        QPointF dy;     // along the side, inwards
    };
    const Corner corners[] = {{box.topLeft(), {1, 0}, {0, 1}},
                              {box.topRight(), {-1, 0}, {0, 1}},
                              {box.bottomRight(), {-1, 0}, {0, -1}},
                              {box.bottomLeft(), {1, 0}, {0, -1}}};
    const double in = width / 2;    // each arm runs inside the box by half its width, so nothing is clipped
    painter.setPen(QPen(accent, width, Qt::SolidLine, Qt::FlatCap));
    for (const Corner &corner : corners) {
        painter.drawLine(corner.at + corner.dy * in, corner.at + corner.dy * in + corner.dx * length);
        painter.drawLine(corner.at + corner.dx * in, corner.at + corner.dx * in + corner.dy * length);
    }
}

} // namespace

QString panelChromeName(PanelChrome chrome) {
    switch (chrome) {
    case PanelChrome::Flat: return u"Flat"_s;
    case PanelChrome::Brackets: return u"Brackets"_s;
    case PanelChrome::Glow: return u"Glow"_s;
    case PanelChrome::Chamfer: return u"Chamfer"_s;
    case PanelChrome::Glass: return u"Glass"_s;
    case PanelChrome::Rail: return u"Rail"_s;
    }
    return {};
}

QString panelChromeKey(PanelChrome chrome) {
    return panelChromeName(chrome).toLower();
}

QString panelChromeBlurb(PanelChrome chrome) {
    switch (chrome) {
    case PanelChrome::Flat: return u"Rounded panels with a hairline, on the bare ground."_s;
    case PanelChrome::Brackets: return u"Square panels with bracketed corners, as a heads-up display draws them."_s;
    case PanelChrome::Glow: return u"Panels edged in the accent and lit from the inside, as neon."_s;
    case PanelChrome::Chamfer: return u"Panels with their corners cut, the cuts stroked in the accent."_s;
    case PanelChrome::Glass: return u"Translucent panes with a light along the top edge."_s;
    case PanelChrome::Rail: return u"Panels with a bar of the accent down the left edge."_s;
    }
    return {};
}

std::optional<PanelChrome> panelChromeFromKey(QStringView key) {
    for (const PanelChrome chrome : kPanelChromes) {
        if (key == panelChromeKey(chrome)) return chrome;
    }
    return std::nullopt;
}

QString backdropName(Backdrop backdrop) {
    switch (backdrop) {
    case Backdrop::Plain: return u"Plain"_s;
    case Backdrop::Grid: return u"Grid"_s;
    case Backdrop::Dots: return u"Dots"_s;
    case Backdrop::Vignette: return u"Vignette"_s;
    case Backdrop::Horizon: return u"Horizon"_s;
    case Backdrop::Scan: return u"Scan lines"_s;
    }
    return {};
}

QString backdropKey(Backdrop backdrop) {
    switch (backdrop) {
    case Backdrop::Scan: return u"scan"_s;
    default: return backdropName(backdrop).toLower();
    }
}

std::optional<Backdrop> backdropFromKey(QStringView key) {
    for (const Backdrop backdrop : kBackdrops) {
        if (key == backdropKey(backdrop)) return backdrop;
    }
    return std::nullopt;
}

void paintBackdrop(QPainter &painter, const QRectF &rect, Backdrop backdrop, const ChromeColours &colours) {
    if (backdrop == Backdrop::Plain) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, false);
    painter.setBrush(Qt::NoBrush);
    switch (backdrop) {
    case Backdrop::Plain: break;
    case Backdrop::Grid:
        grid(painter, rect, colours.line, 28, 0.30, 0.55);
        glowBehindMiddle(painter, rect, colours.accent, 0.11);
        break;
    case Backdrop::Dots:
        dots(painter, rect, colours.line, 22, 0.60);
        glowBehindMiddle(painter, rect, colours.accent, 0.09);
        break;
    case Backdrop::Vignette:
        glowBehindMiddle(painter, rect, colours.accent, 0.16);
        vignette(painter, rect, 0.40);
        break;
    case Backdrop::Horizon:
        painter.setRenderHint(QPainter::Antialiasing, true);
        glowBehindMiddle(painter, rect, colours.accent, 0.10, 0.62);
        horizon(painter, rect, colours.accent, 0.22);
        break;
    case Backdrop::Scan:
        scanLines(painter, rect, colours.line, 0.22);
        glowBehindMiddle(painter, rect, colours.accent, 0.08);
        break;
    }
    painter.restore();
}

double panelRadius(PanelChrome chrome, double designRadius) {
    switch (chrome) {
    case PanelChrome::Flat:
    case PanelChrome::Glow:
    case PanelChrome::Glass: return designRadius;
    case PanelChrome::Brackets: return std::min(designRadius, 2.0);
    case PanelChrome::Chamfer: return 0;
    case PanelChrome::Rail: return std::min(designRadius, 6.0);
    }
    return designRadius;
}

QPainterPath panelOutline(const QRectF &rect, PanelChrome chrome, double radius) {
    QPainterPath path;
    if (chrome == PanelChrome::Chamfer) {
        const double cut = std::clamp(std::min(rect.width(), rect.height()) * 0.12, 6.0, 14.0);
        path.moveTo(rect.left() + cut, rect.top());
        path.lineTo(rect.right() - cut, rect.top());
        path.lineTo(rect.right(), rect.top() + cut);
        path.lineTo(rect.right(), rect.bottom() - cut);
        path.lineTo(rect.right() - cut, rect.bottom());
        path.lineTo(rect.left() + cut, rect.bottom());
        path.lineTo(rect.left(), rect.bottom() - cut);
        path.lineTo(rect.left(), rect.top() + cut);
        path.closeSubpath();
        return path;
    }
    const double r = panelRadius(chrome, radius);
    path.addRoundedRect(rect, r, r);
    return path;
}

void paintPanelChrome(QPainter &painter, const QRectF &rect, PanelChrome chrome, double radius, const ChromeColours &colours) {
    if (chrome == PanelChrome::Flat) return;
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    const QRectF box = rect.adjusted(0.5, 0.5, -0.5, -0.5);
    const double r = panelRadius(chrome, radius);
    switch (chrome) {
    case PanelChrome::Flat: break;
    case PanelChrome::Brackets: {
        painter.setPen(QPen(colours.line, 1));
        painter.setBrush(colours.panel);
        painter.drawRoundedRect(box, r, r);
        const double length = std::clamp(std::min(rect.width(), rect.height()) * 0.18, 8.0, 16.0);
        brackets(painter, box, length, 2.0, colours.accent);
        break;
    }
    case PanelChrome::Glow: {
        painter.setPen(Qt::NoPen);
        painter.setBrush(colours.panel);
        painter.drawRoundedRect(box, r, r);
        // Lit from the inside: bands of the accent fading inwards from the edge.
        constexpr int kBands = 9;
        for (int i = kBands; i >= 1; --i) {
            const QRectF band = box.adjusted(i, i, -i, -i);
            painter.setPen(QPen(withAlpha(colours.accent, 0.22 * (1.0 - double(i) / (kBands + 1))), 1.2));
            painter.setBrush(Qt::NoBrush);
            painter.drawRoundedRect(band, std::max(0.0, r - i), std::max(0.0, r - i));
        }
        painter.setPen(QPen(withAlpha(colours.accent, 0.95), 1.2));
        painter.setBrush(Qt::NoBrush);
        painter.drawRoundedRect(box, r, r);
        break;
    }
    case PanelChrome::Chamfer: {
        const QPainterPath outline = panelOutline(box, chrome, radius);
        painter.setPen(QPen(colours.line, 1));
        painter.setBrush(colours.panel);
        painter.drawPath(outline);
        // The cuts and a short run of each edge beside them, in the accent.
        const double cut = std::clamp(std::min(rect.width(), rect.height()) * 0.12, 6.0, 14.0);
        const double run = cut * 0.9;
        painter.setPen(QPen(colours.accent, 2.0, Qt::SolidLine, Qt::FlatCap, Qt::MiterJoin));
        painter.setBrush(Qt::NoBrush);
        const QRectF b = box.adjusted(0.5, 0.5, -0.5, -0.5);
        const QPolygonF corners[] = {
            {QPointF(b.left() + cut + run, b.top()), QPointF(b.left() + cut, b.top()), QPointF(b.left(), b.top() + cut),
             QPointF(b.left(), b.top() + cut + run)},
            {QPointF(b.right() - cut - run, b.top()), QPointF(b.right() - cut, b.top()), QPointF(b.right(), b.top() + cut),
             QPointF(b.right(), b.top() + cut + run)},
            {QPointF(b.right() - cut - run, b.bottom()), QPointF(b.right() - cut, b.bottom()),
             QPointF(b.right(), b.bottom() - cut), QPointF(b.right(), b.bottom() - cut - run)},
            {QPointF(b.left() + cut + run, b.bottom()), QPointF(b.left() + cut, b.bottom()),
             QPointF(b.left(), b.bottom() - cut), QPointF(b.left(), b.bottom() - cut - run)},
        };
        for (const QPolygonF &corner : corners) painter.drawPolyline(corner);
        break;
    }
    case PanelChrome::Glass: {
        const QPainterPath pane = panelOutline(box, chrome, radius);
        painter.setPen(QPen(withAlpha(colours.line, 0.9), 1));
        painter.setBrush(withAlpha(colours.panel, 0.80));
        painter.drawPath(pane);
        // The light along the top edge, falling off down the pane.
        painter.setClipPath(pane);
        QLinearGradient light(box.topLeft(), QPointF(box.left(), box.top() + box.height() * 0.45));
        light.setColorAt(0.0, withAlpha(colours.text, 0.09));
        light.setColorAt(1.0, withAlpha(colours.text, 0.0));
        painter.fillRect(box, light);
        painter.setPen(QPen(withAlpha(colours.text, 0.28), 1));
        painter.drawLine(QPointF(box.left() + r, box.top() + 1), QPointF(box.right() - r, box.top() + 1));
        painter.setClipping(false);
        break;
    }
    case PanelChrome::Rail: {
        painter.setPen(QPen(colours.line, 1));
        painter.setBrush(colours.panel);
        painter.drawRoundedRect(box, r, r);
        painter.setPen(Qt::NoPen);
        painter.setBrush(colours.accent);
        const double inset = std::max(r, 3.0);
        painter.drawRoundedRect(QRectF(box.left() + 1, box.top() + inset, 3, box.height() - 2 * inset), 1.5, 1.5);
        break;
    }
    }
    painter.restore();
}

} // namespace nexa
