#pragma once

#include "voice/pipeline/VoicePipeline.h"

#include <QToolButton>

namespace nexa {

// The microphone button next to Send. Click: listen until the pause after the request (click
// again to finish early); press and hold: push-to-talk. Shows the input level while listening and
// the pipeline state as its colour; pressing it while Nexa talks cuts the speech.
class MicButton : public QToolButton {
    Q_OBJECT
public:
    explicit MicButton(VoicePipeline *voice, QWidget *parent = nullptr);

protected:
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void paintEvent(QPaintEvent *event) override;

private:
    void showState(VoiceState state);

    VoicePipeline *voice_;
    VoiceState state_{VoiceState::Idle};
    double level_{0.0};
    bool held_{false};              // the button is down and started this utterance
    bool stopOnRelease_{false};     // it was pressed while already listening: finish the request
};

} // namespace nexa
