#include "ui/NexaIcons.h"

#include "ui/NexaCorePainter.h"

#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace nexa {

namespace {

constexpr double kPi = 3.14159265358979323846;

QPointF polar(double degrees, double radius) {
    const double radians = degrees * kPi / 180.0;
    return {radius * std::cos(radians), radius * std::sin(radians)};
}

} // namespace

QIcon NexaIcons::icon(Kind kind, const QColor &colour, int size) {
    QIcon icon;
    for (const qreal ratio : {1.0, 2.0}) {
        QPixmap pixmap(QSize(size, size) * ratio);
        pixmap.setDevicePixelRatio(ratio);
        pixmap.fill(Qt::transparent);
        QPainter painter(&pixmap);
        paint(painter, kind, QRectF(0, 0, size, size), colour);
        icon.addPixmap(pixmap);
    }
    return icon;
}

void NexaIcons::paint(QPainter &p, Kind kind, const QRectF &box, const QColor &colour) {
    const double s = std::min(box.width(), box.height());
    const QPointF c = box.center();
    p.save();
    p.setRenderHint(QPainter::Antialiasing, true);
    QPen pen(colour, std::max(1.2, 0.09 * s));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.translate(c);
    switch (kind) {
    case Kind::Mic:
        NexaCorePainter::paintNodeIcon(p, VoiceNode, QPointF(0, 0), s * 0.9, colour);
        break;
    case Kind::Pc:
        NexaCorePainter::paintNodeIcon(p, PcNode, QPointF(0, 0), s * 0.9, colour);
        break;
    case Kind::Tools:
        NexaCorePainter::paintNodeIcon(p, ToolsNode, QPointF(0, 0), s * 0.9, colour);
        break;
    case Kind::Phone:
        NexaCorePainter::paintNodeIcon(p, PhoneNode, QPointF(0, 0), s * 0.9, colour);
        break;
    case Kind::Send:
        p.drawLine(QPointF(-0.36 * s, 0), QPointF(0.36 * s, 0));
        p.drawPolyline(QPolygonF() << QPointF(0.08 * s, -0.28 * s) << QPointF(0.36 * s, 0) << QPointF(0.08 * s, 0.28 * s));
        break;
    case Kind::Stop:
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawRoundedRect(QRectF(-0.28 * s, -0.28 * s, 0.56 * s, 0.56 * s), 0.08 * s, 0.08 * s);
        break;
    case Kind::Sun:
        p.drawEllipse(QPointF(0, 0), 0.17 * s, 0.17 * s);
        for (int i = 0; i < 8; ++i) {
            p.drawLine(polar(45.0 * i, 0.28 * s), polar(45.0 * i, 0.4 * s));
        }
        break;
    case Kind::Moon: {
        QPainterPath moon;
        moon.addEllipse(QPointF(0, 0), 0.36 * s, 0.36 * s);
        QPainterPath bite;
        bite.addEllipse(QPointF(0.16 * s, -0.12 * s), 0.3 * s, 0.3 * s);
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawPath(moon.subtracted(bite));
        break;
    }
    case Kind::Gear: {
        p.setPen(QPen(colour, std::max(1.4, 0.13 * s)));
        p.drawEllipse(QPointF(0, 0), 0.2 * s, 0.2 * s);
        p.setPen(QPen(colour, std::max(1.8, 0.14 * s), Qt::SolidLine, Qt::RoundCap));
        for (int i = 0; i < 8; ++i) {
            p.drawLine(polar(45.0 * i, 0.3 * s), polar(45.0 * i, 0.42 * s));
        }
        break;
    }
    case Kind::Refresh: {
        const QRectF arc(-0.32 * s, -0.32 * s, 0.64 * s, 0.64 * s);
        p.drawArc(arc, 30 * 16, 290 * 16);
        const QPointF tip = polar(-30.0, 0.32 * s);
        p.drawPolyline(QPolygonF() << (tip + QPointF(-0.2 * s, -0.02 * s)) << tip << (tip + QPointF(0.0, -0.2 * s)));
        break;
    }
    case Kind::Eye: {
        // An almond of two arcs and a round pupil: "this model sees images".
        QPainterPath almond;
        almond.moveTo(-0.38 * s, 0);
        almond.quadTo(0, -0.34 * s, 0.38 * s, 0);
        almond.quadTo(0, 0.34 * s, -0.38 * s, 0);
        p.drawPath(almond);
        p.setBrush(colour);
        p.drawEllipse(QPointF(0, 0), 0.09 * s, 0.09 * s);
        break;
    }
    case Kind::NewChat:
        p.drawLine(QPointF(-0.3 * s, 0), QPointF(0.3 * s, 0));
        p.drawLine(QPointF(0, -0.3 * s), QPointF(0, 0.3 * s));
        break;
    case Kind::Actions:
        for (int i = -1; i <= 1; ++i) {
            const double y = 0.24 * s * i;
            p.drawLine(QPointF(-0.1 * s, y), QPointF(0.36 * s, y));
            p.setBrush(colour);
            p.drawEllipse(QPointF(-0.3 * s, y), 0.05 * s, 0.05 * s);
            p.setBrush(Qt::NoBrush);
        }
        break;
    case Kind::More:
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        for (int i = -1; i <= 1; ++i) p.drawEllipse(QPointF(0, 0.26 * s * i), 0.07 * s, 0.07 * s);
        break;
    case Kind::Pause:
        p.setPen(Qt::NoPen);
        p.setBrush(colour);
        p.drawRoundedRect(QRectF(-0.26 * s, -0.3 * s, 0.17 * s, 0.6 * s), 0.04 * s, 0.04 * s);
        p.drawRoundedRect(QRectF(0.09 * s, -0.3 * s, 0.17 * s, 0.6 * s), 0.04 * s, 0.04 * s);
        break;
    case Kind::Play: {
        QPainterPath play;
        play.moveTo(-0.2 * s, -0.32 * s);
        play.lineTo(0.32 * s, 0);
        play.lineTo(-0.2 * s, 0.32 * s);
        play.closeSubpath();
        p.setBrush(colour);
        p.drawPath(play);
        break;
    }
    case Kind::Check:
        p.drawPolyline(QPolygonF{QPointF(-0.3 * s, 0.02 * s), QPointF(-0.08 * s, 0.24 * s), QPointF(0.32 * s, -0.22 * s)});
        break;
    case Kind::Cross:
        p.drawLine(QPointF(-0.24 * s, -0.24 * s), QPointF(0.24 * s, 0.24 * s));
        p.drawLine(QPointF(0.24 * s, -0.24 * s), QPointF(-0.24 * s, 0.24 * s));
        break;
    case Kind::Globe:
        p.drawEllipse(QPointF(0, 0), 0.36 * s, 0.36 * s);
        p.drawEllipse(QPointF(0, 0), 0.15 * s, 0.36 * s);
        p.drawLine(QPointF(-0.36 * s, 0), QPointF(0.36 * s, 0));
        break;
    case Kind::Code:
        p.drawPolyline(QPolygonF{QPointF(-0.12 * s, -0.26 * s), QPointF(-0.36 * s, 0), QPointF(-0.12 * s, 0.26 * s)});
        p.drawPolyline(QPolygonF{QPointF(0.12 * s, -0.26 * s), QPointF(0.36 * s, 0), QPointF(0.12 * s, 0.26 * s)});
        p.drawLine(QPointF(0.06 * s, -0.32 * s), QPointF(-0.06 * s, 0.32 * s));
        break;
    case Kind::Screen:
        p.drawRoundedRect(QRectF(-0.38 * s, -0.3 * s, 0.76 * s, 0.48 * s), 0.06 * s, 0.06 * s);
        p.drawLine(QPointF(-0.16 * s, 0.34 * s), QPointF(0.16 * s, 0.34 * s));
        break;
    case Kind::Queue:
        for (int i = -1; i <= 1; ++i) {
            const double y = 0.22 * s * i;
            p.drawLine(QPointF(-0.34 * s, y), QPointF(0.34 * s, y));
        }
        break;
    case Kind::Plus:
        p.drawLine(QPointF(-0.28 * s, 0), QPointF(0.28 * s, 0));
        p.drawLine(QPointF(0, -0.28 * s), QPointF(0, 0.28 * s));
        break;
    case Kind::Attach: {
        // A paper clip: a long loop with a shorter one inside, leaning right.
        p.save();
        p.rotate(-40);
        QPainterPath clip;
        clip.moveTo(-0.1 * s, 0.3 * s);
        clip.lineTo(-0.1 * s, -0.2 * s);
        clip.arcTo(QRectF(-0.1 * s, -0.36 * s, 0.32 * s, 0.32 * s), 180, -180);
        clip.lineTo(0.22 * s, 0.26 * s);
        clip.arcTo(QRectF(0.0 * s, 0.15 * s, 0.22 * s, 0.22 * s), 0, -180);
        clip.lineTo(0.0 * s, -0.12 * s);
        clip.arcTo(QRectF(0.0 * s, -0.2 * s, 0.12 * s, 0.16 * s), 180, -180);
        clip.lineTo(0.12 * s, 0.16 * s);
        p.drawPath(clip);
        p.restore();
        break;
    }
    case Kind::Lock: {
        p.drawRoundedRect(QRectF(-0.3 * s, -0.04 * s, 0.6 * s, 0.42 * s), 0.07 * s, 0.07 * s);
        QPainterPath shackle;
        shackle.moveTo(-0.17 * s, -0.04 * s);
        shackle.lineTo(-0.17 * s, -0.17 * s);
        shackle.arcTo(QRectF(-0.17 * s, -0.34 * s, 0.34 * s, 0.34 * s), 180, -180);
        shackle.lineTo(0.17 * s, -0.04 * s);
        p.drawPath(shackle);
        p.drawLine(QPointF(0, 0.1 * s), QPointF(0, 0.22 * s));
        break;
    }
    }
    p.restore();
}

} // namespace nexa
