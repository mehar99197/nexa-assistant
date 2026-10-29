#pragma once

#include "voice/listen/IAudioFilter.h"
#include "voice/listen/IVoiceDetector.h"
#include "voice/wake/IWakeSpotter.h"

#include <QByteArray>
#include <QObject>

#include <memory>

class QThread;

namespace nexa {

class FrontWorker;

// What the pipeline hears in the microphone's audio (Facade): the noise filter cleans it, then the voice
// detector says where speech starts and ends and the spotter says where the wake name was. Runs inline (cheap
// parts, tests) or on a worker thread of its own (the models), reporting back on the thread it was made on.
// Positions are samples since the last reset(); events still queued from before a reset are dropped.
class ListeningFront : public QObject {
    Q_OBJECT
public:
    enum class Threading { Inline, Worker };

    struct Parts {
        std::unique_ptr<IVoiceDetector> detector;   // required
        std::unique_ptr<IWakeSpotter> spotter;      // optional: the wake name
        std::unique_ptr<IAudioFilter> filter;       // optional: noise reduction
    };

    ListeningFront(Parts parts, Threading threading, QObject *parent = nullptr);
    ~ListeningFront() override;

    QString detectorName() const { return detectorName_; }
    bool hasSpotter() const { return hasSpotter_; }
    bool reducesNoise() const { return reducesNoise_; }

    void feed(const QByteArray &pcm16k);    // raw microphone audio
    void reset();
    void setEndPauseMs(int ms);
    void setMaxUtteranceMs(int ms);

signals:
    void speechStarted(qint64 start);
    void speechEnded(qint64 start, qint64 end);
    void wakeHeard(const QString &label, qint64 phraseStart, qint64 heardAt);

private:
    friend class FrontWorker;
    void deliver(quint64 generation, const QList<IVoiceDetector::Event> &events, const QList<IWakeSpotter::Hit> &hits);

    std::unique_ptr<FrontWorker> inline_;   // Threading::Inline
    FrontWorker *worker_{nullptr};          // either the inline one, or one living on thread_
    QThread *thread_{nullptr};
    quint64 generation_{0};
    QString detectorName_;
    bool hasSpotter_{false};
    bool reducesNoise_{false};
};

} // namespace nexa
