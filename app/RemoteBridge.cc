#include "app/RemoteBridge.h"

#include "app/AppController.h"
#include "core/Logging.h"
#include "voice/VoiceBackends.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"
#include "voice/tts/ISpeechSynthesizer.h"
#include "voice/tts/TtsFileRenderer.h"

#include <QPointer>

#include <algorithm>

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
    Result<std::unique_ptr<ISpeechRecognizer>> recognizer = VoiceBackends::createRecognizer(config_, http_);
    if (recognizer) {
        recognizer_ = std::move(recognizer).value();
    } else {
        qCInfo(lcVoice).noquote() << "remote: no speech recognition for the phone:" << recognizer.error().message;
    }
    renderer_ = std::make_unique<TtsFileRenderer>(VoiceBackends::createSynthesizers(config_, http_));
}

bool RemoteBridge::isBusy() const {
    // Requests are never refused for another one running: each is a task, and waits for a slot on the board.
    return false;
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

bool RemoteBridge::isLocked() const {
    const HardcoreMode *hardcore = controller_->hardcore();
    if (!hardcore) return false;
    using State = HardcoreMode::State;
    return hardcore->state() == State::Counting || hardcore->holdsInput();
}

QString RemoteBridge::lockText() const {
    const HardcoreMode *hardcore = controller_->hardcore();
    return hardcore && isLocked() ? hardcore->banner() : QString();
}

void RemoteBridge::unlock() {
    controller_->unlockHardcore(u"phone"_s);
}

QList<RemoteApproval> RemoteBridge::approvals() const {
    QList<RemoteApproval> waiting;
    for (const Approval &approval : controller_->mission()->approvals()) {
        waiting.append(RemoteApproval{approval.actionId, approval.agentId, approval.tool, approval.question});
    }
    return waiting;
}

void RemoteBridge::answer(quint64 id, bool allow) {
    MissionControl *mission = controller_->mission();
    const QList<Approval> waiting = mission->approvals();
    const bool known = std::any_of(waiting.cbegin(), waiting.cend(), [id](const Approval &a) { return a.actionId == id; });
    if (!known) return;         // answered already, from the PC or another phone
    if (allow) {
        mission->approve(id);
    } else {
        mission->deny(id);
    }
}

} // namespace nexa
