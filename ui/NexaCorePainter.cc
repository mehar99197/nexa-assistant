#include "ui/NexaCorePainter.h"

#include <QConicalGradient>
#include <QFont>
#include <QPainter>
#include <QPainterPath>
#include <QPen>
#include <QRadialGradient>

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace nexa {

namespace {

constexpr double kPi = 3.14159265358979323846;

// The nodes' places on the rim: Voice at the top, PC to the right, Tools below, Phone to the left.
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

QColor lighter(const QColor &colour, double amount) {
    return QColor::fromHslF(std::max(0.0, double(colour.hslHueF())), colour.hslSaturationF(),
                            std::clamp(double(colour.lightnessF()) + amount, 0.0, 1.0));
}

// Degrees clockwise from three o'clock, as the screen shows them (y grows downward).
QPointF polar(double degrees, double radius) {
    const double radians = degrees * kPi / 180.0;
    return {radius * std::cos(radians), radius * std::sin(radians)};
}

// A colour turned around the hue circle (Aurora), keeping its saturation and lightness.
QColor hueShifted(const QColor &colour, double degrees) {
    if (std::abs(degrees) < 0.01) return colour;
    const double hue = std::fmod(std::fmod(colour.hslHueF() * 360.0 + degrees, 360.0) + 360.0, 360.0);
    QColor shifted = QColor::fromHslF(hue / 360.0, colour.hslSaturationF(), colour.lightnessF());
    shifted.setAlphaF(colour.alphaF());
    return shifted;
}

// Everything is measured from one reference radius: the rim sits at 0.80 of it.
struct Geometry {
    QPointF centre;
    double unit;
    double core;        // the core's resting radius
};

Geometry layout(const QRectF &rect, const CoreGeometry &geometry) {
    const double radius = std::min(rect.width(), rect.height()) / 2.0;
    const double fit = geometry.nodes ? (geometry.labels ? 1.2 : 0.99) : 0.88;
    const double unit = radius / fit;
    return {rect.center(), unit, 0.31 * unit};
}

QRectF squareAround(const QPointF &centre, double radius) {
    return {centre.x() - radius, centre.y() - radius, 2 * radius, 2 * radius};
}

QPolygonF regularPolygon(const QPointF &centre, double radius, int sides, double rotation) {
    QPolygonF polygon;
    for (int i = 0; i < sides; ++i) polygon << centre + polar(rotation + 360.0 * i / sides, radius);
    return polygon;
}

// The N: two stems and a diagonal, one filled shape, in a disc of radius r.
QPainterPath glyphPath(double r) {
    QPainterPath n;
    n.setFillRule(Qt::WindingFill);
    n.addRect(QRectF(-0.38 * r, -0.44 * r, 0.16 * r, 0.88 * r));
    n.addRect(QRectF(0.22 * r, -0.44 * r, 0.16 * r, 0.88 * r));
    QPolygonF diagonal;
    diagonal << QPointF(-0.38 * r, -0.44 * r) << QPointF(-0.17 * r, -0.44 * r) << QPointF(0.38 * r, 0.44 * r)
             << QPointF(0.17 * r, 0.44 * r);
    n.addPolygon(diagonal);
    return n.simplified();
}

void paintGlyph(QPainter &p, const QPointF &centre, double r, const QColor &colour, bool outlined) {
    p.save();
    p.translate(centre);
    if (outlined) {
        QPen pen(colour, std::max(1.0, 0.05 * r));
        pen.setJoinStyle(Qt::MiterJoin);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
    } else {
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
    }
    p.drawPath(glyphPath(r));
    p.restore();
}

// ------------------------------------------------------------------ the shapes
//
// A shape says where its rim is, draws its rim, its body and its nodes, and bends the overlays
// to its geometry. The overlays' numbers come from the frame; the shape only places them.

class Shape {
public:
    Shape(const Geometry &g, const CorePaint &paint, const CoreGeometry &geometry)
        : g_(g), paint_(paint), geometry_(geometry) {}
    virtual ~Shape() = default;

    // The rim: where the comet runs and the nodes sit. Default: a circle of 0.80 unit.
    virtual double rimRadius(double) const { return 0.80 * g_.unit; }
    QPointF onRim(double angle) const { return g_.centre + polar(angle, rimRadius(angle)); }
    virtual QPointF nodeCentre(double angle) const { return onRim(angle); }

    virtual void halo(QPainter &p, const QColor &accent, double scale) const {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.07));
        p.drawEllipse(g_.centre, 0.60 * g_.unit * scale, 0.60 * g_.unit * scale);
        p.setBrush(withAlpha(accent, 0.12));
        p.drawEllipse(g_.centre, 0.49 * g_.unit * scale, 0.49 * g_.unit * scale);
    }

    virtual void rim(QPainter &p, double angle, const QColor &colour) const {
        QPen pen(colour, std::max(1.2, 0.012 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        pen.setDashPattern({0.5, 3.0});
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.save();
        p.translate(g_.centre);
        p.rotate(angle);
        p.drawEllipse(QPointF(0, 0), 0.80 * g_.unit, 0.80 * g_.unit);
        p.restore();
    }

    virtual void body(QPainter &p, double scale, const QColor &accent) const {
        const double r = g_.core * scale;
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(g_.centre, r, r);
        p.setBrush(withAlpha(QColor(Qt::white), 0.22));
        p.drawEllipse(g_.centre + QPointF(-0.38 * r, -0.38 * r), 0.2 * r, 0.2 * r);
        paintGlyph(p, g_.centre, r, paint_.glyph, false);
    }

    // One node: a disc with its icon; lit, in the action colour.
    virtual void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const {
        const double radius = 0.115 * g_.unit;
        if (lit) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(paint_.act, 0.22));
            p.drawEllipse(centre, radius * 1.55, radius * 1.55);
        }
        p.setPen(QPen(lit ? paint_.act : paint_.line, std::max(1.0, 0.008 * g_.unit)));
        p.setBrush(lit ? paint_.act : paint_.panel);
        p.drawEllipse(centre, radius, radius);
        if (radius >= 5.5) NexaCorePainter::paintNodeIcon(p, which, centre, radius * 1.05, lit ? paint_.ground : paint_.muted);
    }

    void nodes(QPainter &p, CoreNodes lit) const {
        QFont font = p.font();
        font.setPixelSize(std::max(9, int(0.075 * g_.unit)));
        for (const auto &[which, angle] : kNodeAngles) {
            const QPointF centre = nodeCentre(angle);
            node(p, which, centre, (lit & which) != 0);
            if (!geometry_.labels) continue;
            p.setFont(font);
            p.setPen(paint_.muted);
            const double offset = 0.115 * g_.unit + 0.035 * g_.unit;
            QRectF box(0, 0, 0.6 * g_.unit, font.pixelSize() * 1.4);
            const double y = which == ToolsNode ? centre.y() - offset - box.height() / 2 : centre.y() + offset + box.height() / 2;
            box.moveCenter(QPointF(centre.x(), y));
            p.drawText(box, Qt::AlignCenter, coreNodeName(which));
        }
    }

    // Two rings of 48 ticks around the core.
    virtual void ticks(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const {
        if (frame.tickSwell <= 0) return;
        const double shortest = 0.016 * g_.unit;
        const double longest = 0.084 * g_.unit;
        QPen pen(withAlpha(colour, 0.9 * opacity), std::max(1.0, 0.011 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        for (int ring = 0; ring < 2; ++ring) {
            const double base = ring == 0 ? 0.355 * g_.unit : 0.46 * g_.unit;
            const double phase = ring == 0 ? frame.tickPhaseInner : frame.tickPhaseOuter;
            for (int i = 0; i < kTickCount; ++i) {
                const double angle = 360.0 * i / kTickCount;
                const double wave = 0.6 + 0.4 * std::sin(phase + i * 2 * kPi * 3 / kTickCount);
                const double length = shortest + (longest - shortest) * frame.tickSwell * wave;
                p.drawLine(g_.centre + polar(angle, base), g_.centre + polar(angle, base + length));
            }
        }
    }

    virtual void scan(QPainter &p, double angle, const QColor &colour, double opacity) const {
        const QRectF box = squareAround(g_.centre, 0.45 * g_.unit);
        QPen pen(withAlpha(colour, opacity), std::max(2.0, 0.028 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(box, int(-angle * 16), -80 * 16);
        pen.setColor(withAlpha(colour, 0.25 * opacity));
        p.setPen(pen);
        p.drawArc(box, int((-angle + 80) * 16), -60 * 16);
    }

    virtual void satellites(QPainter &p, const std::array<double, 3> &angles, const QColor &colour, double opacity) const {
        constexpr std::array<double, 3> orbits{0.40, 0.47, 0.55};
        p.setBrush(Qt::NoBrush);
        for (const double orbit : orbits) {
            p.setPen(QPen(withAlpha(colour, 0.18 * opacity), 1.0));
            p.drawEllipse(g_.centre, orbit * g_.unit, orbit * g_.unit);
        }
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        for (size_t i = 0; i < orbits.size(); ++i) {
            p.drawEllipse(g_.centre + polar(angles[i] - 90.0, orbits[i] * g_.unit), 0.026 * g_.unit, 0.026 * g_.unit);
        }
    }

    virtual void sonar(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const {
        if (frame.sonarRadius <= 0 || frame.sonarOpacity <= 0) return;
        const double radius = frame.sonarRadius * g_.core;
        p.setPen(QPen(withAlpha(colour, frame.sonarOpacity * opacity), std::max(1.5, 0.012 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, radius, radius);
    }

    virtual void comet(QPainter &p, double angle, const QColor &colour, double opacity) const {
        // The tail: the rim's path over the 40° behind the head, fading out.
        QPen pen(withAlpha(colour, 0.55 * opacity), std::max(1.5, 0.016 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        QPolygonF tail;
        for (int i = 0; i <= 12; ++i) tail << onRim(angle - 40.0 + 40.0 * i / 12);
        for (int i = 0; i < 12; ++i) {
            pen.setColor(withAlpha(colour, 0.85 * opacity * i / 12.0));
            p.setPen(pen);
            p.drawLine(tail[i], tail[i + 1]);
        }
        p.setPen(Qt::NoPen);
        const QPointF head = onRim(angle);
        p.setBrush(withAlpha(colour, opacity));
        p.drawEllipse(head, 0.022 * g_.unit, 0.022 * g_.unit);
        p.setBrush(withAlpha(colour, 0.25 * opacity));
        p.drawEllipse(head, 0.05 * g_.unit, 0.05 * g_.unit);
    }

    virtual void packets(QPainter &p, double progress, CoreNodes lit, const QColor &colour, double opacity) const {
        for (const auto &[which, angle] : kNodeAngles) {
            if (!(lit & which)) continue;
            const QPointF target = nodeCentre(angle);
            const QPointF start = g_.centre + (target - g_.centre) * (g_.core * 1.08 / rimRadius(angle));
            const QPointF end = g_.centre + (target - g_.centre) * (1.0 - 0.135 * g_.unit / rimRadius(angle));
            const QPointF dot = start + (end - start) * progress;
            QPen pen(withAlpha(colour, 0.35 * opacity), std::max(1.0, 0.014 * g_.unit));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(start + (dot - start) * std::max(0.0, 1.0 - 0.25 / std::max(progress, 0.001)) , dot);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(colour, opacity));
            p.drawEllipse(dot, 0.026 * g_.unit, 0.026 * g_.unit);
        }
    }

protected:
    Geometry g_;
    CorePaint paint_;
    CoreGeometry geometry_;
};

// A sphere: glow instead of halo, a shaded disc, nodes as sparks, the N drawn in outline.
class OrbShape : public Shape {
public:
    using Shape::Shape;
    void halo(QPainter &p, const QColor &accent, double scale) const override {
        QRadialGradient glow(g_.centre, 0.78 * g_.unit * scale);
        glow.setColorAt(0.0, withAlpha(accent, 0.30));
        glow.setColorAt(0.45, withAlpha(accent, 0.10));
        glow.setColorAt(1.0, withAlpha(accent, 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(glow);
        p.drawEllipse(g_.centre, 0.78 * g_.unit * scale, 0.78 * g_.unit * scale);
    }
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        // A faint orbit only; the sparks ride on it.
        Q_UNUSED(angle);
        p.setPen(QPen(withAlpha(colour, 0.35), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, 0.80 * g_.unit, 0.80 * g_.unit);
    }
    void body(QPainter &p, double scale, const QColor &accent) const override {
        const double r = g_.core * scale;
        QRadialGradient shade(g_.centre + QPointF(-0.3 * r, -0.3 * r), 1.5 * r);
        shade.setColorAt(0.0, lighter(accent, 0.22));
        shade.setColorAt(0.55, accent);
        shade.setColorAt(1.0, lighter(accent, -0.25));
        p.setPen(Qt::NoPen);
        p.setBrush(shade);
        p.drawEllipse(g_.centre, r, r);
        p.setBrush(withAlpha(QColor(Qt::white), 0.35));
        p.drawEllipse(g_.centre + QPointF(-0.36 * r, -0.42 * r), 0.26 * r, 0.16 * r);
        paintGlyph(p, g_.centre, r, paint_.glyph, true);
    }
    void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const override {
        Q_UNUSED(which);
        const double radius = 0.05 * g_.unit;
        QRadialGradient spark(centre, radius * 2.4);
        spark.setColorAt(0.0, withAlpha(lit ? paint_.act : paint_.muted, lit ? 0.9 : 0.6));
        spark.setColorAt(1.0, withAlpha(lit ? paint_.act : paint_.muted, 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(spark);
        p.drawEllipse(centre, radius * 2.4, radius * 2.4);
        p.setBrush(lit ? paint_.act : paint_.muted);
        p.drawEllipse(centre, radius, radius);
    }
};

// Regular polygons: the rim, the body and the sonar are the polygon; nodes are tiles on its corners.
class PolygonShape : public Shape {
public:
    PolygonShape(const Geometry &g, const CorePaint &paint, const CoreGeometry &geometry, int sides, double rotation)
        : Shape(g, paint, geometry), sides_(sides), rotation_(rotation) {}
    double rimRadius(double degrees) const override {
        return NexaCorePainter::polygonRadiusAt(degrees, 0.80 * g_.unit, sides_, rotation_);
    }
    void halo(QPainter &p, const QColor &accent, double scale) const override {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.07));
        p.drawPolygon(regularPolygon(g_.centre, 0.62 * g_.unit * scale, sides_, rotation_));
        p.setBrush(withAlpha(accent, 0.12));
        p.drawPolygon(regularPolygon(g_.centre, 0.50 * g_.unit * scale, sides_, rotation_));
    }
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        QPen pen(colour, std::max(1.2, 0.012 * g_.unit));
        pen.setCapStyle(Qt::FlatCap);
        pen.setDashPattern({4.0, 3.0});
        pen.setDashOffset(angle / 10.0);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(regularPolygon(g_.centre, 0.80 * g_.unit, sides_, rotation_));
    }
    void body(QPainter &p, double scale, const QColor &accent) const override {
        const double r = g_.core * scale * 1.08;
        const QPolygonF outline = regularPolygon(g_.centre, r, sides_, rotation_);
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawPolygon(outline);
        // Facets: every other wedge a shade lighter, the rest a shade darker.
        for (int i = 0; i < sides_; ++i) {
            QPolygonF wedge;
            wedge << g_.centre << outline[i] << outline[(i + 1) % sides_];
            p.setBrush(withAlpha(i % 2 == 0 ? QColor(Qt::white) : QColor(Qt::black), 0.10));
            p.drawPolygon(wedge);
        }
        p.setBrush(withAlpha(QColor(Qt::white), 0.18));
        p.drawPolygon(regularPolygon(g_.centre + QPointF(-0.3 * r, -0.3 * r), 0.22 * r, sides_, rotation_));
        paintGlyph(p, g_.centre, r * 0.92, paint_.glyph, false);
    }
    void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const override {
        const double radius = 0.12 * g_.unit;
        if (lit) {
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(paint_.act, 0.22));
            p.drawPolygon(regularPolygon(centre, radius * 1.5, sides_, rotation_));
        }
        p.setPen(QPen(lit ? paint_.act : paint_.line, std::max(1.0, 0.008 * g_.unit)));
        p.setBrush(lit ? paint_.act : paint_.panel);
        p.drawPolygon(regularPolygon(centre, radius, sides_, rotation_));
        if (radius >= 5.5) NexaCorePainter::paintNodeIcon(p, which, centre, radius * 0.95, lit ? paint_.ground : paint_.muted);
    }
    void sonar(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.sonarRadius <= 0 || frame.sonarOpacity <= 0) return;
        p.setPen(QPen(withAlpha(colour, frame.sonarOpacity * opacity), std::max(1.5, 0.012 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawPolygon(regularPolygon(g_.centre, frame.sonarRadius * g_.core, sides_, rotation_));
    }
    void satellites(QPainter &p, const std::array<double, 3> &angles, const QColor &colour, double opacity) const override {
        constexpr std::array<double, 3> orbits{0.42, 0.50, 0.58};
        p.setBrush(Qt::NoBrush);
        for (const double orbit : orbits) {
            p.setPen(QPen(withAlpha(colour, 0.18 * opacity), 1.0));
            p.drawPolygon(regularPolygon(g_.centre, orbit * g_.unit, sides_, rotation_));
        }
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        for (size_t i = 0; i < orbits.size(); ++i) {
            const double a = angles[i] - 90.0;
            const QPointF dot = g_.centre + polar(a, NexaCorePainter::polygonRadiusAt(a, orbits[i] * g_.unit, sides_, rotation_));
            p.drawPolygon(regularPolygon(dot, 0.03 * g_.unit, sides_, rotation_));
        }
    }

private:
    int sides_;
    double rotation_;
};

// An iris: striations between the pupil and the rim, always there, lit and lengthened by the ticks.
class IrisShape : public Shape {
public:
    using Shape::Shape;
    void halo(QPainter &p, const QColor &accent, double scale) const override {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.10));
        p.drawEllipse(g_.centre, 0.58 * g_.unit * scale, 0.58 * g_.unit * scale);
        // The striations at rest.
        QPen pen(withAlpha(paint_.muted, 0.45), std::max(1.0, 0.008 * g_.unit));
        p.setPen(pen);
        for (int i = 0; i < 60; ++i) {
            const double angle = 6.0 * i;
            const double length = (i % 2 == 0 ? 0.14 : 0.09) * g_.unit;
            p.drawLine(g_.centre + polar(angle, 0.36 * g_.unit), g_.centre + polar(angle, 0.36 * g_.unit + length));
        }
        p.setPen(QPen(withAlpha(accent, 0.5), std::max(1.0, 0.01 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, 0.58 * g_.unit, 0.58 * g_.unit);
    }
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        Q_UNUSED(angle);
        p.setPen(QPen(withAlpha(colour, 0.6), std::max(1.0, 0.01 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, 0.80 * g_.unit, 0.80 * g_.unit);
    }
    void ticks(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.tickSwell <= 0) return;
        QPen pen(withAlpha(colour, 0.9 * opacity), std::max(1.0, 0.011 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        for (int i = 0; i < 60; ++i) {
            const double angle = 6.0 * i;
            const double wave = 0.6 + 0.4 * std::sin(frame.tickPhaseInner + i * 2 * kPi * 3 / 60.0);
            const double length = 0.08 * g_.unit + 0.14 * g_.unit * frame.tickSwell * wave;
            p.drawLine(g_.centre + polar(angle, 0.36 * g_.unit), g_.centre + polar(angle, 0.36 * g_.unit + length));
        }
    }
    void body(QPainter &p, double scale, const QColor &accent) const override {
        const double r = g_.core * scale;
        QRadialGradient pupil(g_.centre, r);
        pupil.setColorAt(0.0, lighter(accent, -0.3));
        pupil.setColorAt(0.7, accent);
        pupil.setColorAt(1.0, lighter(accent, 0.1));
        p.setPen(Qt::NoPen);
        p.setBrush(pupil);
        p.drawEllipse(g_.centre, r, r);
        p.setBrush(withAlpha(QColor(Qt::white), 0.3));
        p.drawEllipse(g_.centre + QPointF(-0.3 * r, -0.35 * r), 0.16 * r, 0.16 * r);
        paintGlyph(p, g_.centre, r * 0.9, paint_.glyph, false);
    }
    void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const override {
        // An arc on the rim, with the icon outside it.
        const double angle = std::atan2(centre.y() - g_.centre.y(), centre.x() - g_.centre.x()) * 180.0 / kPi;
        QPen pen(lit ? paint_.act : paint_.line, std::max(2.0, 0.03 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        p.drawArc(squareAround(g_.centre, 0.80 * g_.unit), int(-(angle - 14.0) * 16), -28 * 16);
        const double radius = 0.1 * g_.unit;
        if (radius >= 5.0) {
            NexaCorePainter::paintNodeIcon(p, which, g_.centre + polar(angle, 0.80 * g_.unit), radius * 1.4,
                                           lit ? paint_.act : paint_.muted);
        }
    }
};

// A gauge: a 270° arc open at the bottom; the ring angle is its sweep, the ticks a meter along it.
class ArcShape : public Shape {
public:
    using Shape::Shape;
    static constexpr double kStart = 135.0;     // degrees, clockwise from three o'clock
    static constexpr double kSpan = 270.0;
    double rimRadius(double) const override { return 0.76 * g_.unit; }
    // Angles map onto the gauge, so the comet and the nodes stay on it.
    static double onGauge(double degrees) {
        const double turn = std::fmod(std::fmod(degrees + 90.0, 360.0) + 360.0, 360.0);   // 0 at the top
        return kStart + kSpan * 0.5 + (turn <= 180.0 ? turn : turn - 360.0) * kSpan / 360.0;
    }
    QPointF nodeCentre(double angle) const override {
        if (angle == 90.0) return g_.centre + QPointF(0, 0.60 * g_.unit);    // Tools sits in the gap
        return g_.centre + polar(onGauge(angle), 0.76 * g_.unit);
    }
    void halo(QPainter &p, const QColor &accent, double scale) const override {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.08));
        p.drawEllipse(g_.centre, 0.52 * g_.unit * scale, 0.52 * g_.unit * scale);
    }
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        const QRectF box = squareAround(g_.centre, 0.76 * g_.unit);
        QPen track(withAlpha(paint_.line, 0.9), std::max(3.0, 0.07 * g_.unit));
        track.setCapStyle(Qt::RoundCap);
        p.setPen(track);
        p.setBrush(Qt::NoBrush);
        p.drawArc(box, int(-kStart * 16), int(-kSpan * 16));
        // The sweep: as far round as the ring has turned.
        QPen sweep(withAlpha(colour, 0.9), std::max(3.0, 0.07 * g_.unit));
        sweep.setCapStyle(Qt::RoundCap);
        p.setPen(sweep);
        const double swept = kSpan * std::fmod(angle, 360.0) / 360.0;
        p.drawArc(box, int(-kStart * 16), int(-swept * 16));
        // Gauge marks.
        p.setPen(QPen(withAlpha(paint_.muted, 0.6), 1.0));
        for (int i = 0; i <= 18; ++i) {
            const double a = kStart + kSpan * i / 18.0;
            p.drawLine(g_.centre + polar(a, 0.84 * g_.unit), g_.centre + polar(a, (i % 3 == 0 ? 0.90 : 0.87) * g_.unit));
        }
    }
    void ticks(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.tickSwell <= 0) return;
        // A VU meter: segments along the gauge light up as far as the level swells.
        const int segments = 36;
        const int lit = int(std::round(segments * std::clamp(frame.tickSwell, 0.0, 1.0)));
        QPen pen(withAlpha(colour, 0.9 * opacity), std::max(2.0, 0.035 * g_.unit));
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        for (int i = 0; i < lit; ++i) {
            const double a = kStart + kSpan * (i + 0.5) / segments;
            const double wobble = 0.03 * g_.unit * std::sin(frame.tickPhaseInner + i * 0.5);
            p.drawLine(g_.centre + polar(a, 0.52 * g_.unit), g_.centre + polar(a, 0.62 * g_.unit + wobble));
        }
    }
    void comet(QPainter &p, double angle, const QColor &colour, double opacity) const override {
        // A needle at the comet's place on the gauge.
        const double a = kStart + kSpan * std::fmod(angle, 360.0) / 360.0;
        QPen pen(withAlpha(colour, opacity), std::max(2.0, 0.03 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(g_.centre + polar(a, 0.40 * g_.unit), g_.centre + polar(a, 0.70 * g_.unit));
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        p.drawEllipse(g_.centre + polar(a, 0.76 * g_.unit), 0.03 * g_.unit, 0.03 * g_.unit);
    }
    void satellites(QPainter &p, const std::array<double, 3> &angles, const QColor &colour, double opacity) const override {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        constexpr std::array<double, 3> radii{0.44, 0.52, 0.60};
        for (size_t i = 0; i < radii.size(); ++i) {
            const double a = kStart + kSpan * (0.5 + 0.5 * std::sin(angles[i] * kPi / 180.0));
            p.drawEllipse(g_.centre + polar(a, radii[i] * g_.unit), 0.026 * g_.unit, 0.026 * g_.unit);
        }
    }
    void sonar(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.sonarRadius <= 0 || frame.sonarOpacity <= 0) return;
        p.setPen(QPen(withAlpha(colour, frame.sonarOpacity * opacity), std::max(1.5, 0.012 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawArc(squareAround(g_.centre, frame.sonarRadius * g_.core), int(-kStart * 16), int(-kSpan * 16));
    }
};

// A flat dot: the core and a hairline ring, nothing else unless the state asks.
class DotShape : public Shape {
public:
    using Shape::Shape;
    void halo(QPainter &, const QColor &, double) const override {}
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        Q_UNUSED(angle);
        p.setPen(QPen(withAlpha(colour, 0.7), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, 0.80 * g_.unit, 0.80 * g_.unit);
    }
    void body(QPainter &p, double scale, const QColor &accent) const override {
        const double r = g_.core * scale;
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawEllipse(g_.centre, r, r);
        p.setPen(QPen(withAlpha(accent, 0.5), 1.0));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, r * 1.22, r * 1.22);
        paintGlyph(p, g_.centre, r * 0.9, paint_.glyph, true);
    }
    void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const override {
        Q_UNUSED(which);
        p.setPen(Qt::NoPen);
        p.setBrush(lit ? paint_.act : paint_.muted);
        const double radius = (lit ? 0.045 : 0.03) * g_.unit;
        p.drawEllipse(centre, radius, radius);
    }
    void ticks(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.tickSwell <= 0) return;
        // One ring that grows with the level.
        const double radius = (0.38 + 0.22 * frame.tickSwell) * g_.unit;
        p.setPen(QPen(withAlpha(colour, 0.8 * opacity), std::max(1.0, 0.012 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawEllipse(g_.centre, radius, radius);
    }
};

// A meter: the core as a rounded square to the left, bars for the level to its right, the nodes
// as a column of squares. Angles become positions along the bars; everything else stays flat.
class BarsShape : public Shape {
public:
    using Shape::Shape;
    static constexpr int kBars = 14;
    double left() const { return g_.centre.x() - 0.08 * g_.unit; }
    double right() const { return g_.centre.x() + 0.72 * g_.unit; }
    double baseline() const { return g_.centre.y() + 0.62 * g_.unit; }
    QPointF coreCentre() const { return g_.centre + QPointF(-0.50 * g_.unit, 0.0); }
    double along(double angle) const { return left() + (right() - left()) * std::fmod(std::fmod(angle, 360.0) + 360.0, 360.0) / 360.0; }
    QPointF nodeCentre(double angle) const override {
        const double x = g_.centre.x() + 0.90 * g_.unit;
        const int row = angle == -90.0 ? 0 : angle == 0.0 ? 1 : angle == 90.0 ? 2 : 3;
        return {x, g_.centre.y() + (-0.54 + 0.36 * row) * g_.unit};
    }
    void halo(QPainter &p, const QColor &accent, double scale) const override {
        const double r = 0.42 * g_.unit * scale;
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(accent, 0.10));
        p.drawRoundedRect(squareAround(coreCentre(), r), 0.25 * r, 0.25 * r);
    }
    void rim(QPainter &p, double angle, const QColor &colour) const override {
        // The baseline under the bars, dotted, its dots marching with the ring.
        QPen pen(colour, std::max(1.2, 0.012 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        pen.setDashPattern({0.5, 3.0});
        pen.setDashOffset(-angle / 20.0);
        p.setPen(pen);
        p.drawLine(QPointF(left(), baseline()), QPointF(right(), baseline()));
        // The bars at rest.
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(paint_.line, 0.9));
        const double width = (right() - left()) / kBars;
        for (int i = 0; i < kBars; ++i) {
            p.drawRoundedRect(QRectF(left() + i * width + width * 0.2, baseline() - 0.06 * g_.unit, width * 0.6, 0.05 * g_.unit),
                              1.5, 1.5);
        }
    }
    void body(QPainter &p, double scale, const QColor &accent) const override {
        const double r = g_.core * scale;
        p.setPen(Qt::NoPen);
        p.setBrush(accent);
        p.drawRoundedRect(squareAround(coreCentre(), r), 0.28 * r, 0.28 * r);
        p.setBrush(withAlpha(QColor(Qt::white), 0.18));
        p.drawRoundedRect(QRectF(coreCentre().x() - 0.7 * r, coreCentre().y() - 0.75 * r, 1.4 * r, 0.35 * r), 0.15 * r, 0.15 * r);
        paintGlyph(p, coreCentre(), r * 0.95, paint_.glyph, false);
    }
    void node(QPainter &p, CoreNode which, const QPointF &centre, bool lit) const override {
        const double half = 0.11 * g_.unit;
        p.setPen(QPen(lit ? paint_.act : paint_.line, std::max(1.0, 0.008 * g_.unit)));
        p.setBrush(lit ? paint_.act : paint_.panel);
        p.drawRoundedRect(squareAround(centre, half), 0.2 * half, 0.2 * half);
        if (half >= 5.5) NexaCorePainter::paintNodeIcon(p, which, centre, half * 1.05, lit ? paint_.ground : paint_.muted);
    }
    void ticks(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.tickSwell <= 0) return;
        p.setPen(Qt::NoPen);
        const double width = (right() - left()) / kBars;
        for (int i = 0; i < kBars; ++i) {
            const double wave = 0.5 + 0.5 * std::sin(frame.tickPhaseInner + i * 0.9) * std::sin(frame.tickPhaseOuter * 0.5 + i * 0.4);
            const double height = (0.08 + 1.05 * frame.tickSwell * wave) * g_.unit;
            p.setBrush(withAlpha(colour, opacity * (0.55 + 0.45 * wave)));
            p.drawRoundedRect(QRectF(left() + i * width + width * 0.2, baseline() - height, width * 0.6, height), 1.5, 1.5);
        }
    }
    void scan(QPainter &p, double angle, const QColor &colour, double opacity) const override {
        // A bright window sweeping along the bars.
        const double x = along(angle);
        const double w = 0.18 * g_.unit;
        QLinearGradient sweep(QPointF(x - w, 0), QPointF(x + w, 0));
        sweep.setColorAt(0.0, withAlpha(colour, 0.0));
        sweep.setColorAt(0.5, withAlpha(colour, 0.6 * opacity));
        sweep.setColorAt(1.0, withAlpha(colour, 0.0));
        p.setPen(Qt::NoPen);
        p.setBrush(sweep);
        p.drawRect(QRectF(x - w, g_.centre.y() - 0.6 * g_.unit, 2 * w, 1.22 * g_.unit));
    }
    void satellites(QPainter &p, const std::array<double, 3> &angles, const QColor &colour, double opacity) const override {
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        for (size_t i = 0; i < angles.size(); ++i) {
            const double x = left() + (right() - left()) * (0.5 + 0.5 * std::sin(angles[i] * kPi / 180.0));
            const double y = g_.centre.y() - (0.2 + 0.18 * i) * g_.unit;
            p.drawEllipse(QPointF(x, y), 0.026 * g_.unit, 0.026 * g_.unit);
        }
    }
    void sonar(QPainter &p, const CoreFrame &frame, const QColor &colour, double opacity) const override {
        if (frame.sonarRadius <= 0 || frame.sonarOpacity <= 0) return;
        const double grow = (frame.sonarRadius - 0.4) / 2.5;
        const QRectF box(coreCentre().x() - g_.core * (1 + grow * 0.6), g_.centre.y() - g_.core * (1 + grow * 1.4),
                         2 * g_.core * (1 + grow * 0.6), 2 * g_.core * (1 + grow * 1.4));
        p.setPen(QPen(withAlpha(colour, frame.sonarOpacity * opacity), std::max(1.5, 0.012 * g_.unit)));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(box, 0.2 * g_.core, 0.2 * g_.core);
    }
    void comet(QPainter &p, double angle, const QColor &colour, double opacity) const override {
        const double x = along(angle);
        QLinearGradient tail(QPointF(x - 0.2 * g_.unit, 0), QPointF(x, 0));
        tail.setColorAt(0.0, withAlpha(colour, 0.0));
        tail.setColorAt(1.0, withAlpha(colour, 0.8 * opacity));
        QPen pen(QBrush(tail), std::max(1.5, 0.02 * g_.unit));
        pen.setCapStyle(Qt::RoundCap);
        p.setPen(pen);
        p.drawLine(QPointF(std::max(left(), x - 0.2 * g_.unit), baseline()), QPointF(x, baseline()));
        p.setPen(Qt::NoPen);
        p.setBrush(withAlpha(colour, opacity));
        p.drawEllipse(QPointF(x, baseline()), 0.024 * g_.unit, 0.024 * g_.unit);
    }
    void packets(QPainter &p, double progress, CoreNodes lit, const QColor &colour, double opacity) const override {
        for (const auto &[which, angle] : kNodeAngles) {
            if (!(lit & which)) continue;
            const QPointF start = coreCentre() + QPointF(g_.core * 1.1, 0);
            const QPointF end = nodeCentre(angle) - QPointF(0.14 * g_.unit, 0);
            const QPointF dot = start + (end - start) * progress;
            QPen pen(withAlpha(colour, 0.35 * opacity), std::max(1.0, 0.014 * g_.unit));
            pen.setCapStyle(Qt::RoundCap);
            p.setPen(pen);
            p.drawLine(start + (dot - start) * 0.6, dot);
            p.setPen(Qt::NoPen);
            p.setBrush(withAlpha(colour, opacity));
            p.drawEllipse(dot, 0.026 * g_.unit, 0.026 * g_.unit);
        }
    }
};

std::unique_ptr<Shape> shapeFor(const Geometry &g, const CorePaint &paint, const CoreGeometry &geometry) {
    switch (geometry.shape) {
    case CoreShape::Ring: return std::make_unique<Shape>(g, paint, geometry);
    case CoreShape::Orb: return std::make_unique<OrbShape>(g, paint, geometry);
    case CoreShape::Hex: return std::make_unique<PolygonShape>(g, paint, geometry, 6, 0.0);
    case CoreShape::Bars: return std::make_unique<BarsShape>(g, paint, geometry);
    case CoreShape::Iris: return std::make_unique<IrisShape>(g, paint, geometry);
    case CoreShape::Crystal: return std::make_unique<PolygonShape>(g, paint, geometry, 8, 22.5);
    case CoreShape::Arc: return std::make_unique<ArcShape>(g, paint, geometry);
    case CoreShape::Dot: return std::make_unique<DotShape>(g, paint, geometry);
    }
    return std::make_unique<Shape>(g, paint, geometry);
}

} // namespace

double NexaCorePainter::polygonRadiusAt(double degrees, double radius, int sides, double rotation) {
    const double step = 2 * kPi / sides;
    const double apothem = radius * std::cos(step / 2);
    double theta = (degrees - rotation) * kPi / 180.0;
    theta = std::fmod(std::fmod(theta, step) + step, step) - step / 2;    // offset from the nearest edge's middle
    return apothem / std::cos(theta);
}

void NexaCorePainter::paint(QPainter &p, const QRectF &rect, const CorePaint &paint, const CoreGeometry &geometry,
                            CoreNodes lit, double coreScale, std::span<const CoreLayer> layers) {
    const Geometry g = layout(rect, geometry);
    const std::unique_ptr<Shape> shape = shapeFor(g, paint, geometry);
    p.setRenderHint(QPainter::Antialiasing, true);

    // The rim turns with whichever state shows most; it is the privacy ring while the wake word
    // keeps the microphone open. A style may widen the halo or drop the rim.
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
    shape->halo(p, paint.accent, haloScale);
    if (ringOpacity > 0.01) {
        shape->rim(p, ringAngle, withAlpha(mix(withAlpha(paint.line, 0.9), paint.wake, wake), 0.9 * ringOpacity));
    }

    for (const CoreLayer &layer : layers) {
        if (layer.opacity <= 0.01) continue;
        const QColor colour = hueShifted(layer.colour, layer.frame.hueShift);
        shape->sonar(p, layer.frame, colour, layer.opacity);
        shape->ticks(p, layer.frame, colour, layer.opacity);
        if (layer.frame.scanAngle) shape->scan(p, *layer.frame.scanAngle, colour, layer.opacity);
        if (layer.frame.satelliteAngles) shape->satellites(p, *layer.frame.satelliteAngles, colour, layer.opacity);
        if (layer.frame.cometAngle) shape->comet(p, *layer.frame.cometAngle, colour, layer.opacity);
        if (layer.frame.secondCometAngle) shape->comet(p, *layer.frame.secondCometAngle, colour, layer.opacity * 0.8);
    }

    if (geometry.nodes) shape->nodes(p, lit);
    shape->body(p, coreScale, paint.accent);

    for (const CoreLayer &layer : layers) {
        if (layer.opacity <= 0.01 || !layer.frame.packetProgress) continue;
        shape->packets(p, *layer.frame.packetProgress, lit, paint.act, layer.opacity);
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
