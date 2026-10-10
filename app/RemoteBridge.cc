#include "app/RemoteBridge.h"

#include "app/AppController.h"
#include "core/Logging.h"
#include "voice/VoiceBackends.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"
#include "voice/tts/ISpeechSynthesizer.h"
#include "voice/tts/TtsFileRenderer.h"

#include <QPointer>

using namespace Qt::StringLiterals;

namespace nexa {

RemoteBridge::RemoteBridge(AppController *controller, HttpClient *http, QObject *parent)
    : QObject(parent), controller_(controller), http_(http), config_(controller->config()) {}

RemoteBridge::~RemoteBridge() = default;

void RemoteBridge::applyConfig(const Config &config) {
    config_ = config;
    built_ = false;
    recognizer_.reset();
    renderer_.reset();
    transcribing_ = false;
}

void RemoteBridge::build() const {
    if (built_) return;
    built_ = true;
    Result<std::unique_ptr<ISpeechRecognizer>> recognizer =
        VoiceBackends::createRecognizer(config_.voice, config_.active(), http_);
    if (recognizer) {
        recognizer_ = std::move(recognizer).value();
    } else {
        qCInfo(lcVoice).noquote() << "remote: no speech recognition for the phone:" << recognizer.error().message;
    }
    renderer_ = std::make_unique<TtsFileRenderer>(
        VoiceBackends::createSynthesizers(config_.voice, config_.active(), http_));
}

bool RemoteBridge::isBusy() const {
    return controller_->isBusy();
}

void RemoteBridge::submit(const QString &text, const QString &source) {
    controller_->submit(text, source);
}

void RemoteBridge::cancel() {
    controller_->cancel();
}

bool RemoteBridge::canTranscribe() const {
    build();
    return recognizer_ != nullptr;
}

void RemoteBridge::transcribe(const QByteArray &pcm16k, std::function<void(Result<QString>)> done) {
    build();
    if (!recognizer_) {
        done(Error{ErrorCode::NotSupported, u"Speech recognition is not set up on the PC (Settings → Voice)."_s});
        return;
    }
    // One recording at a time: two phones speaking at once would otherwise share one recogniser.
    if (transcribing_) {
        done(Error{ErrorCode::Unknown, u"Another recording is being transcribed; try again in a moment."_s});
        return;
    }
    transcribing_ = true;
    const QPointer<RemoteBridge> self(this);
    recognizer_->transcribe(pcm16k, QString(), [self, done](Result<QString> text) {
        if (self) self->transcribing_ = false;
        if (!text) {
            done(text.error());
            return;
        }
        done(cleanTranscript(text.value()));
    });
}

void RemoteBridge::renderSpeech(const QString &text, std::function<void(Result<QByteArray>)> done) {
    build();
    if (!renderer_ || renderer_->isEmpty()) {
        done(Error{ErrorCode::NotSupported, u"No voice is set up on the PC (Settings → Voice)."_s});
        return;
    }
    renderer_->render(text, std::move(done));
}

} // namespace nexa
