#include "ui/NexaCorePainter.h"

#include <QConicalGradient>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace nexa {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The nodes' places on the ring: Voice at the top, PC to the right, Tools below, Phone to the left.
constexpr std::array<std::pair<CoreNode, double>, 4> kNodeAngles{
    {{VoiceNode, -90.0}, {PcNode, 0.0}, {ToolsNode, 90.0}, {PhoneNode, 180.0}}};

QColor withAlpha(QColor colour, double alpha) {
    colour.setAlphaF(std::clamp(alpha, 0.0, 1.0));
    return colour;
}

QColor mix(const QColor &a, const QColor &b, double t) {
    t = std::clamp(t, 0.0, 1.0);
    return QColor::fromRgbF(a.redF() + (b.redF() - a.redF()) * t, a.greenF() + (b.greenF() - a.greenF()) * t,
                            a.blueF() + (b.blueF() - a.blueF()) * t, a.alphaF() + (b.alphaF() - a.alphaF()) * t);
}

// Degrees clockwise from three o'clock, as the screen shows them (y grows downward).
QPointF polar(double degrees, double radius) {
    const double radians = degrees * kPi / 180.0;
    return {radius * std::cos(radians), radius * std::sin(radians)};
}

// Everything is measured from one reference radius: the dotted ring sits at 0.80 of it.
struct Geometry {
    QPointF centre;
    double unit;
    double core;        // the core's resting radius
};

Geometry layout(const QRectF &rect, const CoreGeometry &geometry) {
    const double radius = std::min(rect.width(), rect.height()) / 2.0;
    // Room for the nodes' glow at the rim, and for their names when they are shown.
    const double fit = geometry.nodes ? (geometry.labels ? 1.2 : 0.99) : 0.88;
    const double unit = radius / fit;
    return {rect.center(), unit, 0.31 * unit};
}

QRectF squareAround(const QPointF &centre, double radius) {
    return {centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius};
}

void paintHalo(QPainter &p, const Geometry &g, const QColor &accent, double scale) {
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(accent, 0.07));
    p.drawEllipse(g.centre, 0.60 * g.unit * scale, 0.60 * g.unit * scale);
    p.setBrush(withAlpha(accent, 0.12));
    p.drawEllipse(g.centre, 0.49 * g.unit * scale, 0.49 * g.unit * scale);
}

// A colour turned around the hue circle (Aurora), keeping its saturation and lightness.
QColor hueShifted(const QColor &colour, double degrees) {
    if (std::abs(degrees) < 0.01) return colour;
    const double hue = std::fmod(std::fmod(colour.hslHueF() * 360.0 + degrees, 360.0) + 360.0, 360.0);
    QColor shifted = QColor::fromHslF(hue / 360.0, colour.hslSaturationF(), colour.lightnessF());
    shifted.setAlphaF(colour.alphaF());
    return shifted;
}

void paintRing(QPainter &p, const Geometry &g, double angle, const QColor &colour) {
    const double radius = 0.80 * g.unit;
    QPen pen(colour, std::max(1.2, 0.012 * g.unit));
    pen.setCapStyle(Qt::RoundCap);
    pen.setDashPattern({0.5, 3.0});
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.save();
    p.translate(g.centre);
    p.rotate(angle);
    p.drawEllipse(QPointF(0, 0), radius, radius);
    p.restore();
}

void paintTicks(QPainter &p, const Geometry &g, const CoreFrame &frame, const QColor &colour, double opacity) {
    if (frame.tickSwell <= 0) return;
    const double shortest = 0.016 * g.unit;
    const double longest = 0.084 * g.unit;
    QPen pen(withAlpha(colour, 0.9 * opacity), std::max(1.0, 0.011 * g.unit));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    for (int ring = 0; ring < 2; ++ring) {
        const double base = ring == 0 ? 0.355 * g.unit : 0.46 * g.unit;
        const double phase = ring == 0 ? frame.tickPhaseInner : frame.tickPhaseOuter;
        for (int i = 0; i < kTickCount; ++i) {
            const double angle = 360.0 * i / kTickCount;
            // Three lobes travel around each ring, so the swell reads as motion, not a pulse.
            const double wave = 0.6 + 0.4 * std::sin(phase + i * 2 * kPi * 3 / kTickCount);
            const double length = shortest + (longest - shortest) * frame.tickSwell * wave;
            p.drawLine(g.centre + polar(angle, base), g.centre + polar(angle, base + length));
        }
    }
}

void paintScan(QPainter &p, const Geometry &g, double angle, const QColor &colour, double opacity) {
    const QRectF box = squareAround(g.centre, 0.45 * g.unit);
    QPen pen(withAlpha(colour, opacity), std::max(2.0, 0.028 * g.unit));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    // QPainter counts degrees counter-clockwise in sixteenths; the arc chases its own tail clockwise.
    p.drawArc(box, int(-angle * 16), -80 * 16);
    pen.setColor(withAlpha(colour, 0.25 * opacity));
    p.setPen(pen);
    p.drawArc(box, int((-angle + 80) * 16), -60 * 16);
}

void paintSatellites(QPainter &p, const Geometry &g, const std::array<double, 3> &angles, const QColor &colour,
                     double opacity) {
    constexpr std::array<double, 3> orbits{0.40, 0.47, 0.55};
    p.setBrush(Qt::NoBrush);
    for (size_t i = 0; i < orbits.size(); ++i) {
        const double radius = orbits[i] * g.unit;
        p.setPen(QPen(withAlpha(colour, 0.18 * opacity), 1.0));
        p.drawEllipse(g.centre, radius, radius);
    }
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(colour, opacity));
    for (size_t i = 0; i < orbits.size(); ++i) {
        const QPointF dot = g.centre + polar(angles[i] - 90.0, orbits[i] * g.unit);
        p.drawEllipse(dot, 0.026 * g.unit, 0.026 * g.unit);
    }
}

void paintSonar(QPainter &p, const Geometry &g, const CoreFrame &frame, const QColor &colour, double opacity) {
    if (frame.sonarRadius <= 0 || frame.sonarOpacity <= 0) return;
    const double radius = frame.sonarRadius * g.core;
    p.setPen(QPen(withAlpha(colour, frame.sonarOpacity * opacity), std::max(1.5, 0.012 * g.unit)));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(g.centre, radius, radius);
}

void paintComet(QPainter &p, const Geometry &g, double angle, const QColor &colour, double opacity) {
    const double radius = 0.80 * g.unit;
    // The tail: a conical gradient fading out behind the head, drawn as a 40° arc.
    QConicalGradient tail(g.centre, -angle);
    tail.setColorAt(0.0, withAlpha(colour, 0.85 * opacity));
    tail.setColorAt(40.0 / 360.0, withAlpha(colour, 0.0));
    tail.setColorAt(1.0, withAlpha(colour, 0.0));
    QPen pen(QBrush(tail), std::max(1.5, 0.016 * g.unit));
    pen.setCapStyle(Qt::RoundCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawArc(squareAround(g.centre, radius), int(-angle * 16), 40 * 16);
    p.setPen(Qt::NoPen);
    p.setBrush(withAlpha(colour, opacity));
    const QPointF head = g.centre + polar(angle, radius);
    p.drawEllipse(head, 0.022 * g.unit, 0.022 * g.unit);
    p.setBrush(withAlpha(colour, 0.25 * opacity));
    p.drawEllipse(head, 0.05 * g.unit, 0.05 * g.unit);
}

void paintNodes(QPainter &p, const Geometry &g, const CorePaint &paint, CoreNodes lit, bool labels) {
    const double radius = 0.115 * g.unit;
    QFont font = p.font();
    font.setPixelSize(std::max(9, int(0.075 * g.unit)));
    for (const auto &[node, angle] : kNodeAngles) {
        const QPointF centre = g.centre + polar(angle, 0.80 * g.unit);
        const bool on = (lit & node) != 0;
        if (on) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(paint.act, 0.22));
            p.drawEllipse(centre, radius * 1.55, radius * 1.55);
        }
        p.setPen(QPen(on ? paint.act : paint.line, std::max(1.0, 0.008 * g.unit)));
        p.setBrush(on ? paint.act : paint.panel);
        p.drawEllipse(centre, radius, radius);
        if (radius >= 5.5) {
            NexaCorePainter::paintNodeIcon(p, node, centre, radius * 1.05, on ? paint.ground : paint.muted);
        }
        if (!labels) continue;
        // "Tools" sits above its node; the other names sit below theirs.
        p.setFont(font);
        p.setPen(paint.muted);
        const double offset = radius + 0.035 * g.unit;
        QRectF box(0, 0, 5 * radius, font.pixelSize() * 1.4);
        const double y = node == ToolsNode ? centre.y() - offset - box.height() / 2 : centre.y() + offset + box.height() / 2;
        box.moveCenter(QPointF(centre.x(), y));
        p.drawText(box, Qt::AlignCenter, coreNodeName(node));
    }
}

void paintCore(QPainter &p, const Geometry &g, double scale, const QColor &accent, const QColor &glyph) {
    const double r = g.core * scale;
    p.setPen(Qt::NoPen);
    p.setBrush(accent);
    p.drawEllipse(g.centre, r, r);
    // A glint at the upper left, so the disc reads as a sphere.
    p.setBrush(withAlpha(QColor(Qt::white), 0.22));
    p.drawEllipse(g.centre + QPointF(-0.38 * r, -0.38 * r), 0.2 * r, 0.2 * r);
    // The N: two stems and a diagonal, one filled shape.
    QPainterPath n;
    n.setFillRule(Qt::WindingFill);
    n.addRect(QRectF(-0.38 * r, -0.44 * r, 0.16 * r, 0.88 * r));
    n.addRect(QRectF(0.22 * r, -0.44 * r, 0.16 * r, 0.88 * r));
    QPolygonF diagonal;
    diagonal << QPointF(-0.38 * r, -0.44 * r) << QPointF(-0.17 * r, -0.44 * r) << QPointF(0.38 * r, 0.44 * r)
             << QPointF(0.17 * r, 0.44 * r);
    n.addPolygon(diagonal);
    p.setBrush(glyph);
    p.save();
    p.translate(g.centre);
    p.drawPath(n.simplified());
    p.restore();
}

void paintPackets(QPainter &p, const Geometry &g, double progress, CoreNodes lit, const QColor &colour,
                  double opacity) {
    const double from = g.core * 1.08;
    const double to = 0.80 * g.unit - 0.135 * g.unit;
    for (const auto &[node, angle] : kNodeAngles) {
        if (!(lit & node)) continue;
        const double distance = from + (to - from) * progress;
        const QPointF dot = g.centre + polar(angle, distance);
        QPen pen(withAlpha(colour, 0.35 * opacity), std::max(1.0, 0.014 * g.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(g.centre + polar(angle, std::max(from, distance - 0.12 * g.unit)), dot);
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        p.drawEllipse(dot, 0.026 * g.unit, 0.026 * g.unit);
    }
}

} // namespace

void NexaCorePainter::paint(QPainter &p, const QRectF &rect, const CorePaint &paint, const CoreGeometry &geometry,
                            CoreNodes lit, double coreScale, std::span<const CoreLayer> layers) {
    const Geometry g = layout(rect, geometry);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The dotted ring turns with whichever state shows most; it is the privacy ring while the
    // wake word keeps the microphone open. A style may widen the halo or drop the ring.
    double ringAngle = 0.0;
    double ringOpacity = 0.0;
    double haloScale = 1.0;
    double wake = 0.0;
    double best = -1.0;
    for (const CoreLayer &layer : layers) {
        if (layer.opacity > best) {
            best = layer.opacity;
            ringAngle = layer.frame.ringAngle;
            haloScale = layer.frame.haloScale;
        }
        if (layer.frame.ring) ringOpacity = std::max(ringOpacity, layer.opacity);
        if (layer.mode == CoreMode::WakeWord) wake = std::max(wake, layer.opacity);
    }
    paintHalo(p, g, paint.accent, haloScale);
    if (ringOpacity > 0.01) {
        paintRing(p, g, ringAngle, withAlpha(mix(withAlpha(paint.line, 0.9), paint.wake, wake), 0.9 * ringOpacity));
    }

    for (const CoreLayer &layer : layers) {
        if (layer.opacity <= 0.01) continue;
        const QColor colour = hueShifted(layer.colour, layer.frame.hueShift);
        paintSonar(p, g, layer.frame, colour, layer.opacity);
        paintTicks(p, g, layer.frame, colour, layer.opacity);
        if (layer.frame.scanAngle) paintScan(p, g, *layer.frame.scanAngle, colour, layer.opacity);
        if (layer.frame.satelliteAngles) paintSatellites(p, g, *layer.frame.satelliteAngles, colour, layer.opacity);
        if (layer.frame.cometAngle) paintComet(p, g, *layer.frame.cometAngle, colour, layer.opacity);
        if (layer.frame.secondCometAngle) paintComet(p, g, *layer.frame.secondCometAngle, colour, layer.opacity * 0.8);
    }

    if (geometry.nodes) paintNodes(p, g, paint, lit, geometry.labels);
    paintCore(p, g, coreScale, paint.accent, paint.glyph);

    for (const CoreLayer &layer : layers) {
        if (layer.opacity <= 0.01 || !layer.frame.packetProgress) continue;
        paintPackets(p, g, *layer.frame.packetProgress, lit, paint.act, layer.opacity);
    }
}

QColor NexaCorePainter::shiftedHue(const QColor &colour, double degrees) {
    return hueShifted(colour, degrees);
}

void NexaCorePainter::paintNodeIcon(QPainter &p, CoreNode node, const QPointF &centre, double size,
                                    const QColor &colour) {
    const double s = size;
    QPen pen(colour, std::max(1.1, 0.1 * s));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.save();
    p.translate(centre);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    switch (node) {
    case VoiceNode:
        p.drawRoundedRect(QRectF(-0.17 * s, -0.46 * s, 0.34 * s, 0.54 * s), 0.17 * s, 0.17 * s);
        p.drawArc(QRectF(-0.34 * s, -0.46 * s, 0.68 * s, 0.68 * s), 180 * 16, 180 * 16);
        p.drawLine(QPointF(0, 0.22 * s), QPointF(0, 0.42 * s));
        p.drawLine(QPointF(-0.16 * s, 0.42 * s), QPointF(0.16 * s, 0.42 * s));
        break;
    case PcNode:
        p.drawRoundedRect(QRectF(-0.44 * s, -0.38 * s, 0.88 * s, 0.56 * s), 0.08 * s, 0.08 * s);
        p.drawLine(QPointF(0, 0.18 * s), QPointF(0, 0.38 * s));
        p.drawLine(QPointF(-0.2 * s, 0.38 * s), QPointF(0.2 * s, 0.38 * s));
        break;
    case ToolsNode: {
        QPolygonF chevron;
        chevron << QPointF(-0.4 * s, -0.3 * s) << QPointF(-0.1 * s, 0) << QPointF(-0.4 * s, 0.3 * s);
        p.drawPolyline(chevron);
        p.drawLine(QPointF(0.05 * s, 0.32 * s), QPointF(0.42 * s, 0.32 * s));
        break;
    }
    case PhoneNode:
        p.drawRoundedRect(QRectF(-0.24 * s, -0.44 * s, 0.48 * s, 0.88 * s), 0.1 * s, 0.1 * s);
        p.drawLine(QPointF(-0.07 * s, 0.3 * s), QPointF(0.07 * s, 0.3 * s));
        break;
    case NoNode:
        break;
    }
    p.restore();
}

} // namespace nexa
