#pragma once

#include "voice/capture/MicrophoneCheck.h"

#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

class VoicePipeline;
class WpctlAudioController;

// "Lower it": steps the default input's volume down until the microphone stops clipping and the room leaves a
// voice some room (Mediator between the volume control and the pipeline's measurements). Waits for the
// amplifier to settle after each change, then measures 2 s. Runs only when the owner presses the button —
// Nexa never changes the input volume on its own.
class MicrophoneLeveler : public QObject {
    Q_OBJECT
public:
    MicrophoneLeveler(WpctlAudioController *microphone, VoicePipeline *voice, QObject *parent = nullptr);

    bool running() const { return running_; }
    // Says, when done, where it stopped and whether the sound is usable now.
    void run(std::function<void(QString summary)> done);

private:
    void measure();
    void finish(const QString &summary);

    WpctlAudioController *microphone_;
    VoicePipeline *voice_;
    std::function<void(QString)> done_;
    bool running_{false};
    int startPercent_{0};
    int percent_{0};
};

} // namespace nexa
