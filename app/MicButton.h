#pragma once

#include "ui/NexaCoreWidget.h"
#include "voice/pipeline/VoicePipeline.h"

namespace nexa {

// The Talk button: the Nexa core, pressed. Click: listen until the pause after the request (click
// again to finish early); press and hold: push-to-talk. It shows the same state as the presence
// beside the chat (MainWindow sets its mode) and swells with the microphone while listening;
// pressing it while Nexa talks cuts the speech.
class MicButton : public NexaCoreButton {
    Q_OBJECT
public:
    explicit MicButton(VoicePipeline *voice, QWidget *parent = nullptr);

private:
    void onPressed();
    void onReleased();

    VoicePipeline *voice_;
    bool stopOnRelease_{false};     // it was pressed while already listening: finish the request
};

} // namespace nexa
