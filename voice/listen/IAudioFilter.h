#pragma once

#include <QByteArray>
#include <QByteArrayView>

namespace nexa {

// Cleans the microphone's audio before the detectors hear it (Strategy) — the noise reducer. Over time it gives
// back as many samples as it was given, at most a frame late. Not a QObject: it runs on the front's thread.
class IAudioFilter {
public:
    virtual ~IAudioFilter() = default;

    virtual QByteArray process(QByteArrayView pcm16k) = 0;
    virtual void reset() = 0;
};

} // namespace nexa
