#include "voice/listen/ListeningFront.h"

#include <QThread>

using namespace Qt::StringLiterals;

namespace nexa {

// Holds the parts and runs them, on whichever thread it lives on.
class FrontWorker : public QObject {
public:
    explicit FrontWorker(ListeningFront::Parts parts) : parts_(std::move(parts)) {}

    void process(const QByteArray &pcm16k, QList<IVoiceDetector::Event> &events, QList<IWakeSpotter::Hit> &hits) {
        const QByteArray heard = parts_.filter ? parts_.filter->process(pcm16k) : pcm16k;
        events = parts_.detector->feed(heard);
        if (parts_.spotter) hits = parts_.spotter->feed(heard);
    }

    void reset() {
        if (parts_.filter) parts_.filter->reset();
        parts_.detector->reset();
        if (parts_.spotter) parts_.spotter->reset();
    }

    IVoiceDetector &detector() { return *parts_.detector; }

private:
    ListeningFront::Parts parts_;
};

ListeningFront::ListeningFront(Parts parts, Threading threading, QObject *parent)
    : QObject(parent),
      detectorName_(parts.detector ? parts.detector->name() : QString()),
      hasSpotter_(parts.spotter != nullptr),
      reducesNoise_(parts.filter != nullptr) {
    Q_ASSERT(parts.detector);
    if (threading == Threading::Inline) {
        inline_ = std::make_unique<FrontWorker>(std::move(parts));
        worker_ = inline_.get();
        return;
    }
    thread_ = new QThread(this);
    thread_->setObjectName(u"nexa-listen"_s);
    worker_ = new FrontWorker(std::move(parts));
    worker_->moveToThread(thread_);
    connect(thread_, &QThread::finished, worker_, &QObject::deleteLater);
    thread_->start();
}

ListeningFront::~ListeningFront() {
    if (!thread_) return;
    // Work already done for this front may sit in the event queue: it goes with this object.
    thread_->quit();
    thread_->wait();
}

void ListeningFront::feed(const QByteArray &pcm16k) {
    const quint64 generation = generation_;
    if (inline_) {
        QList<IVoiceDetector::Event> events;
        QList<IWakeSpotter::Hit> hits;
        inline_->process(pcm16k, events, hits);
        deliver(generation, events, hits);
        return;
    }
    QMetaObject::invokeMethod(worker_, [this, worker = worker_, pcm16k, generation] {
        QList<IVoiceDetector::Event> events;
        QList<IWakeSpotter::Hit> hits;
        worker->process(pcm16k, events, hits);
        if (events.isEmpty() && hits.isEmpty()) return;
        QMetaObject::invokeMethod(this, [this, generation, events, hits] { deliver(generation, events, hits); },
                                  Qt::QueuedConnection);
    }, Qt::QueuedConnection);
}

void ListeningFront::reset() {
    ++generation_;
    if (inline_) {
        inline_->reset();
        return;
    }
    QMetaObject::invokeMethod(worker_, [worker = worker_] { worker->reset(); }, Qt::QueuedConnection);
}

void ListeningFront::setEndPauseMs(int ms) {
    if (inline_) {
        inline_->detector().setEndPauseMs(ms);
        return;
    }
    QMetaObject::invokeMethod(worker_, [worker = worker_, ms] { worker->detector().setEndPauseMs(ms); },
                              Qt::QueuedConnection);
}

void ListeningFront::setMaxUtteranceMs(int ms) {
    if (inline_) {
        inline_->detector().setMaxUtteranceMs(ms);
        return;
    }
    QMetaObject::invokeMethod(worker_, [worker = worker_, ms] { worker->detector().setMaxUtteranceMs(ms); },
                              Qt::QueuedConnection);
}

void ListeningFront::deliver(quint64 generation, const QList<IVoiceDetector::Event> &events,
                             const QList<IWakeSpotter::Hit> &hits) {
    if (generation != generation_) return;      // heard before a reset
    for (const IVoiceDetector::Event &event : events) {
        if (event.kind == IVoiceDetector::Event::Kind::Started) {
            emit speechStarted(event.start);
        } else {
            emit speechEnded(event.start, event.end);
        }
        if (generation != generation_) return;  // a slot reset the front
    }
    for (const IWakeSpotter::Hit &hit : hits) {
        emit wakeHeard(hit.label, hit.phraseStart, hit.heardAt);
        if (generation != generation_) return;
    }
}

} // namespace nexa
