#include "voice/tts/SpeechdTtsBackend.h"

#include <QVoice>

using namespace Qt::StringLiterals;

namespace nexa {

SpeechdTtsBackend::SpeechdTtsBackend(const QString &voice, double rate, QObject *parent)
    : ITextToSpeech(parent), tts_(u"speechd"_s) {
    tts_.setRate(rate);
    if (!voice.isEmpty()) {
        const QList<QVoice> voices = tts_.availableVoices();
        for (const QVoice &candidate : voices) {
            if (candidate.name() == voice) {
                tts_.setVoice(candidate);
                break;
            }
        }
    }

    connect(&tts_, &QTextToSpeech::stateChanged, this, [this](QTextToSpeech::State state) {
        if (state == QTextToSpeech::Speaking) {
            if (speaking_ && !announced_) {
                announced_ = true;
                emit started();
            }
        } else if (state == QTextToSpeech::Ready) {
            if (std::exchange(stopping_, false)) return;    // the end of a stopped queue
            if (std::exchange(speaking_, false)) {
                announced_ = false;
                emit finished();
            }
        }
    });
    connect(&tts_, &QTextToSpeech::errorOccurred, this,
            [this](QTextToSpeech::ErrorReason, const QString &message) {
                speaking_ = false;
                announced_ = false;
                stopping_ = false;
                emit failed(Error{ErrorCode::Io, u"Speech Dispatcher: %1"_s.arg(message)});
            });
}

// The engine reports an error from its own destructor when Speech Dispatcher is not there (a container, a
// server): its signals are cut first, or they would reach a half-destroyed backend.
SpeechdTtsBackend::~SpeechdTtsBackend() {
    tts_.disconnect(this);
}

QString SpeechdTtsBackend::name() const {
    return u"Speech Dispatcher"_s;
}

void SpeechdTtsBackend::speak(const QString &text) {
    if (text.trimmed().isEmpty()) return;
    if (tts_.state() == QTextToSpeech::Error) {
        emit failed(Error{ErrorCode::Io, u"Speech Dispatcher is not available: %1"_s.arg(tts_.errorString())});
        return;
    }
    // A stop() whose Ready already arrived must not swallow this sentence's Ready.
    if (tts_.state() == QTextToSpeech::Ready) stopping_ = false;
    speaking_ = true;
    tts_.enqueue(text);
}

void SpeechdTtsBackend::stop() {
    const QTextToSpeech::State state = tts_.state();
    if (state == QTextToSpeech::Speaking || state == QTextToSpeech::Synthesizing) stopping_ = true;
    speaking_ = false;
    announced_ = false;
    tts_.stop(QTextToSpeech::BoundaryHint::Immediate);
}

QStringList SpeechdTtsBackend::voiceNames() {
    QTextToSpeech engine(u"speechd"_s);
    QStringList names;
    for (const QVoice &voice : engine.availableVoices()) names.append(voice.name());
    names.removeDuplicates();
    return names;
}

} // namespace nexa
