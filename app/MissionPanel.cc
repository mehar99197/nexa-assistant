#include "app/MissionPanel.h"

#include <QPainter>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

MissionPanel::MissionPanel(QWidget *parent) : QFrame(parent) {}

void MissionPanel::set(QColor &member, const QColor &colour) {
    member = colour;
    emit coloursChanged();
    update();
}

QColor MissionPanel::laneColor(Lane lane) const {
    switch (lane) {
    case Lane::Web: return web_;
    case Lane::Code: return code_;
    case Lane::Screen: return screen_;
    case Lane::Phone: return phone_;
    case Lane::Voice: return voice_;
    case Lane::General: return accent_;
    }
    return accent_;
}

QColor MissionPanel::stateColor(TaskState state) const {
    switch (state) {
    case TaskState::Running: return good_;
    case TaskState::AwaitingYou:
    case TaskState::Paused: return warn_;
    case TaskState::Delegating: return code_;
    case TaskState::Failed: return error_;
    case TaskState::Queued:
    case TaskState::Done:
    case TaskState::Stopped: return muted_;
    }
    return muted_;
}

QFont MissionPanel::mono(double points, bool bold) const {
    QFont font(mono_);
    font.setStyleHint(QFont::Monospace);
    font.setPointSizeF(points);
    font.setBold(bold);
    return font;
}

void MissionPanel::paintChip(QPainter &painter, const QRectF &box, const QString &text, const QColor &colour) const {
    painter.save();
    painter.setRenderHint(QPainter::Antialiasing, true);
    QColor fill = colour;
    fill.setAlphaF(0.12f);
    QColor edge = colour;
    edge.setAlphaF(0.55f);
    painter.setPen(QPen(edge, 1));
    painter.setBrush(fill);
    painter.drawRoundedRect(box.adjusted(0.5, 0.5, -0.5, -0.5), 4, 4);
    painter.setPen(colour);
    painter.setFont(mono(7.5, true));
    painter.drawText(box, Qt::AlignCenter, text);
    painter.restore();
}

QString MissionPanel::elapsed(const Task &task) {
    if (!task.started.isValid()) return u"--:--"_s;
    const QDateTime end = task.ended.isValid() ? task.ended : QDateTime::currentDateTime();
    const qint64 seconds = std::max<qint64>(0, task.started.secsTo(end));
    const qint64 h = seconds / 3600;
    const qint64 m = (seconds % 3600) / 60;
    const qint64 s = seconds % 60;
    return h > 0 ? u"%1:%2:%3"_s.arg(h).arg(m, 2, 10, u'0').arg(s, 2, 10, u'0')
                 : u"%1:%2"_s.arg(m, 2, 10, u'0').arg(s, 2, 10, u'0');
}

} // namespace nexa
