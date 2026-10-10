#include "app/MicButton.h"

#include <QMouseEvent>
#include <QPainter>

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QColor stateColor(VoiceState state) {
    switch (state) {
    case VoiceState::Listening: return QColor(0xe5, 0x53, 0x4b);
    case VoiceState::Transcribing: return QColor(0xe0, 0x9b, 0x2d);
    case VoiceState::Thinking: return QColor(0x4a, 0x90, 0xd9);
    case VoiceState::Speaking: return QColor(0x2b, 0xb3, 0xa0);
    case VoiceState::Idle: break;
    }
    return {};
}

} // namespace

MicButton::MicButton(VoicePipeline *voice, QWidget *parent) : QToolButton(parent), voice_(voice) {
    setIcon(QIcon::fromTheme(u"audio-input-microphone"_s));
    setText(tr("Talk"));
    setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
    setMinimumWidth(88);
    setToolTip(tr("Click to speak a request (it ends when you pause), or hold to talk (Ctrl+M)"));
    connect(voice_, &VoicePipeline::stateChanged, this, &MicButton::showState);
    connect(voice_, &VoicePipeline::levelChanged, this, [this](double level) {
        level_ = level;
        update();
    });
}

void MicButton::showState(VoiceState state) {
    state_ = state;
    if (state != VoiceState::Listening) level_ = 0.0;
    switch (state) {
    case VoiceState::Listening: setText(tr("Listening")); break;
    case VoiceState::Transcribing: setText(tr("Hearing…")); break;
    case VoiceState::Thinking: setText(tr("Thinking")); break;
    case VoiceState::Speaking: setText(tr("Speaking")); break;
    case VoiceState::Idle: setText(tr("Talk")); break;
    }
    update();
}

void MicButton::mousePressEvent(QMouseEvent *event) {
    QToolButton::mousePressEvent(event);
    if (event->button() != Qt::LeftButton) return;
    // A press while listening finishes the request; otherwise start right away, so nothing said
    // while the button goes down is lost. VoicePipeline decides on release whether the press was
    // a tap (keep listening until the pause) or a hold (push-to-talk).
    held_ = true;
    stopOnRelease_ = voice_->state() == VoiceState::Listening;
    if (!stopOnRelease_) voice_->startListening(true);
}

void MicButton::mouseReleaseEvent(QMouseEvent *event) {
    QToolButton::mouseReleaseEvent(event);
    if (event->button() != Qt::LeftButton || !std::exchange(held_, false)) return;
    if (stopOnRelease_) {
        voice_->stopListening();
    } else {
        voice_->releaseHold();
    }
}

void MicButton::paintEvent(QPaintEvent *event) {
    QToolButton::paintEvent(event);
    const QColor color = stateColor(state_);
    if (!color.isValid()) return;
    QPainter painter(this);
    painter.setRenderHint(QPainter::Antialiasing);
    painter.setPen(QPen(color, 2));
    painter.setBrush(Qt::NoBrush);
    painter.drawRoundedRect(QRectF(rect()).adjusted(1, 1, -1, -1), 4, 4);
    if (state_ == VoiceState::Listening) {
        // Input level along the bottom edge.
        const QRectF bar(3, height() - 5, (width() - 6) * level_, 3);
        painter.setPen(Qt::NoPen);
        painter.setBrush(color);
        painter.drawRect(bar);
    }
}

} // namespace nexa
