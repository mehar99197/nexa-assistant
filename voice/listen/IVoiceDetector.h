#pragma once

#include <QByteArrayView>
#include <QList>
#include <QString>

namespace nexa {

// Where speech starts and ends in the microphone's audio (Strategy): by loudness (EnergyVoiceDetector) or by
// a model that hears voices (SileroVoiceDetector). Positions are samples since reset(). Not a QObject: the
// listening front runs it on its worker thread.
class IVoiceDetector {
public:
    struct Event {
        enum class Kind { Started, Ended } kind{Kind::Started};
        qint64 start{0};    // Started: where the speech seems to begin; Ended: the utterance's span
        qint64 end{0};
    };

    virtual ~IVoiceDetector() = default;

    virtual QString name() const = 0;
    // 16 kHz mono Int16, any chunking; the events it caused, in order.
    virtual QList<Event> feed(QByteArrayView pcm16k) = 0;
    virtual void reset() = 0;
    virtual void setEndPauseMs(int ms) = 0;         // this much quiet ends an utterance
    virtual void setMaxUtteranceMs(int ms) = 0;     // longer speech is cut here
};

} // namespace nexa
