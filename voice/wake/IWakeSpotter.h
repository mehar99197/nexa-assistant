#pragma once

#include <QByteArrayView>
#include <QList>
#include <QString>

namespace nexa {

// Hears the wake name in the microphone's audio as it streams (Strategy): sherpa-onnx's keyword spotter.
// Positions are samples since reset(). Not a QObject: the listening front runs it on its worker thread.
class IWakeSpotter {
public:
    struct Hit {
        QString label;              // which form was heard: "HEY_NEXA", "NEXA" (WakePhrases::isGreeted)
        qint64 phraseStart{0};      // where the phrase — greeting or name — began
        qint64 heardAt{0};          // where the spotter was when it noticed
    };

    virtual ~IWakeSpotter() = default;

    virtual QList<Hit> feed(QByteArrayView pcm16k) = 0;
    virtual void reset() = 0;
};

} // namespace nexa
