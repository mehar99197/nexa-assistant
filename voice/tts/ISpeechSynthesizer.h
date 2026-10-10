#pragma once

#include "core/Result.h"
#include "voice/Wav.h"

#include <QObject>
#include <QString>

#include <functional>

namespace nexa {

// Text → audio clip (Strategy) for engines that hand back PCM instead of playing it themselves:
// the cloud endpoint and Piper. SynthesizedSpeech chains them and plays the clips.
class ISpeechSynthesizer : public QObject {
    Q_OBJECT
public:
    using Done = std::function<void(Result<PcmAudio>)>;

    using QObject::QObject;

    virtual QString name() const = 0;
    // `done` runs exactly once on the calling thread; after cancel() with ErrorCode::Cancelled.
    virtual void synthesize(const QString &text, Done done) = 0;
    virtual void cancel() = 0;
    // Gets ready to synthesise without delay (starts a local engine); optional.
    virtual void warmUp() {}
};

} // namespace nexa
