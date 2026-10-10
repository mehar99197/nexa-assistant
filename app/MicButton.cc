#include "app/MicButton.h"

using namespace Qt::StringLiterals;

namespace nexa {

MicButton::MicButton(VoicePipeline *voice, QWidget *parent) : NexaCoreButton(parent), voice_(voice) {
    setObjectName(u"talkButton"_s);
    setDiameter(64);
    setAccessibleName(tr("Talk"));
    setToolTip(tr("Click to speak a request (it ends when you pause), or hold to talk (Ctrl+M)"));
    connect(voice_, &VoicePipeline::levelChanged, this, &MicButton::setLevel);
    connect(this, &NexaCoreButton::pressed, this, &MicButton::onPressed);
    connect(this, &NexaCoreButton::released, this, &MicButton::onReleased);
}

void MicButton::onPressed() {
    // A press while listening finishes the request; otherwise start right away, so nothing said
    // while the button goes down is lost. VoicePipeline decides on release whether the press was
    // a tap (keep listening until the pause) or a hold (push-to-talk).
    stopOnRelease_ = voice_->state() == VoiceState::Listening;
    if (!stopOnRelease_) voice_->startListening(true);
}

void MicButton::onReleased() {
    if (stopOnRelease_) {
        voice_->stopListening();
    } else {
        voice_->releaseHold();
    }
}

} // namespace nexa
