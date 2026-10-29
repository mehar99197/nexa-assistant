#pragma once

#include "voice/capture/EnergyVad.h"
#include "voice/listen/IVoiceDetector.h"

namespace nexa {

// Speech by loudness (IVoiceDetector over EnergyVad): what Nexa has without the Silero model. A loud steady
// noise — a fan blowing on the microphone — keeps it from hearing where speech starts and ends.
class EnergyVoiceDetector : public IVoiceDetector {
public:
    explicit EnergyVoiceDetector(double startMarginDb = 14.0);

    QString name() const override { return QStringLiteral("loudness"); }
    QList<Event> feed(QByteArrayView pcm16k) override;
    void reset() override { vad_.reset(); }
    void setEndPauseMs(int ms) override { vad_.setHangoverMs(ms); }
    void setMaxUtteranceMs(int ms) override { vad_.setMaxUtteranceMs(ms); }

private:
    EnergyVad vad_;
};

} // namespace nexa
