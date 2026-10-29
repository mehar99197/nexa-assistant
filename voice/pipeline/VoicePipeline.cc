#include "voice/pipeline/VoicePipeline.h"

#include "core/Logging.h"
#include "voice/capture/IAudioInput.h"
#include "voice/pipeline/WakeWordDetector.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"
#include "voice/tts/ITextToSpeech.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kNoSpeechTimeoutMs = 8'000;       // toggle mode: give up when nothing is said
constexpr int kMaxListenMs = 60'000;
constexpr int kCommandTimeoutMs = 6'000;        // after a bare "Hey Nexa"
constexpr int kTapMs = 350;                     // pressed shorter than this: a tap, not a hold
constexpr qsizetype kMinUtteranceBytes = 16'000 * 2 * 3 / 10;     // 300 ms

EnergyVad::Settings vadSettings() {
    EnergyVad::Settings settings;
    settings.hangoverMs = 900;      // a pause this long ends an utterance
    return settings;
}

} // namespace

VoicePipeline::VoicePipeline(IAudioInput *input, QObject *parent)
    : QObject(parent), input_(input), vad_(vadSettings()) {
    connect(input_, &IAudioInput::audio, this, &VoicePipeline::onAudio);
    connect(input_, &IAudioInput::failed, this, [this](const Error &error) {
        emit notice(error.message, true);
        if (state_ == VoiceState::Listening) {
            recording_.clear();
            setState(VoiceState::Idle);
        }
    });

    listenTimer_.setInterval(250);
    connect(&listenTimer_, &QTimer::timeout, this, [this] {
        if (state_ != VoiceState::Listening) {
            listenTimer_.stop();
            return;
        }
        const qint64 elapsed = listenClock_.elapsed();
        const bool silent = !hold_ && !fromVad_ && !vad_.heardSpeech() && elapsed >= kNoSpeechTimeoutMs;
        if (elapsed >= kMaxListenMs || silent) finishListening();
    });
    commandTimer_.setSingleShot(true);
    commandTimer_.setInterval(kCommandTimeoutMs);
    connect(&commandTimer_, &QTimer::timeout, this, [this] {
        awaitingCommand_ = false;
        if (state_ == VoiceState::Listening && fromVad_) {
            fromVad_ = false;
            setState(VoiceState::Idle);
        }
    });
}

VoicePipeline::~VoicePipeline() {
    // Callbacks the backends deliver while shutting down are dropped by the generation check.
    ++generation_;
    speech_.reset();
    recognizer_.reset();
    wakeRecognizer_.reset();
    input_->disconnect(this);
    input_->stop();
}

void VoicePipeline::setRecognizer(std::unique_ptr<ISpeechRecognizer> recognizer) {
    ++generation_;
    if (recognizer_) recognizer_->cancel();
    recognizer_ = std::move(recognizer);
    if (state_ == VoiceState::Transcribing) setState(VoiceState::Idle);
}

void VoicePipeline::setWakeRecognizer(std::unique_ptr<ISpeechRecognizer> recognizer) {
    ++generation_;
    if (wakeRecognizer_) wakeRecognizer_->cancel();
    wakeBusy_ = false;
    awaitingCommand_ = false;
    wakeRecognizer_ = recognizer && recognizer->isLocal() ? std::move(recognizer) : nullptr;
    if (wakeWordActive()) wakeRecognizer_->warmUp();
    updateMicrophone();
}

void VoicePipeline::setTextToSpeech(std::unique_ptr<ITextToSpeech> speech) {
    if (speech_) speech_->stop();
    speech_ = std::move(speech);
    if (state_ == VoiceState::Speaking) setState(VoiceState::Idle);
    if (!speech_) return;
    connect(speech_.get(), &ITextToSpeech::finished, this, &VoicePipeline::onSpeechFinished);
    connect(speech_.get(), &ITextToSpeech::failed, this, [this](const Error &error) {
        emit notice(tr("Speech output failed: %1").arg(error.message), true);
        if (!speech_->isSpeaking()) onSpeechFinished();
    });
}

void VoicePipeline::setSettings(Settings settings) {
    settings_ = settings;
    wake_ = WakeWordDetector(settings_.wakeName);
    awaitingCommand_ = false;
    if (wakeWordActive()) wakeRecognizer_->warmUp();
    updateMicrophone();
    emit settingsChanged();
}

bool VoicePipeline::wakeWordActive() const {
    return settings_.wakeWord && wakeRecognizer_ && wakeRecognizer_->isLocal();
}

QString VoicePipeline::recognizerName() const {
    return recognizer_ ? recognizer_->name() : QString();
}

QString VoicePipeline::speechName() const {
    return speech_ ? speech_->name() : QString();
}

VoicePipeline::SpeakMode VoicePipeline::speakModeFromString(const QString &mode) {
    if (mode == u"always"_s) return SpeakMode::Always;
    if (mode == u"never"_s) return SpeakMode::Never;
    return SpeakMode::VoiceRequests;
}

void VoicePipeline::toggleListening() {
    if (state_ != VoiceState::Listening) {
        startListening(false);
    } else if (fromVad_) {
        cancel();
    } else {
        stopListening();
    }
}

void VoicePipeline::startListening(bool hold) {
    if (!recognizer_) {
        emit notice(tr("Speech recognition is not set up — see Settings → Voice."), true);
        return;
    }
    if (state_ == VoiceState::Listening && !fromVad_) {
        hold_ = hold_ || hold;
        return;
    }
    ++generation_;
    recognizer_->cancel();          // a transcription in progress is replaced
    recognizer_->warmUp();          // load a local model while the user speaks
    if (speech_) {
        speech_->stop();            // pressing the key cuts the speech
        speech_->warmUp();          // and the voice gets ready for the reply
    }
    chunker_.reset();
    speakReply_ = false;
    awaitingCommand_ = false;
    commandTimer_.stop();
    recording_.clear();
    hold_ = hold;
    fromVad_ = false;
    vad_.reset();
    listenClock_.restart();
    setState(VoiceState::Listening);
    if (state_ != VoiceState::Listening) return;    // the microphone could not be opened
    listenTimer_.start();
    if (replyActive_) emit interruptRequested();    // the new request replaces the running one
}

void VoicePipeline::releaseHold() {
    if (state_ != VoiceState::Listening || fromVad_) return;
    if (listenClock_.elapsed() < kTapMs) {
        hold_ = false;      // tapped, not held: hands-free
        return;
    }
    finishListening();
}

void VoicePipeline::stopListening() {
    if (state_ == VoiceState::Listening && !fromVad_) finishListening();
}

void VoicePipeline::cancel() {
    ++generation_;
    listenTimer_.stop();
    commandTimer_.stop();
    recording_.clear();
    hold_ = false;
    fromVad_ = false;
    awaitingCommand_ = false;
    wakeBusy_ = false;
    if (recognizer_) recognizer_->cancel();
    if (wakeRecognizer_) wakeRecognizer_->cancel();
    if (speech_) speech_->stop();
    chunker_.reset();
    speakReply_ = false;
    setState(VoiceState::Idle);
}

void VoicePipeline::speak(const QString &text) {
    if (!speech_) return;
    SentenceChunker chunker;
    QStringList sentences = chunker.feed(text);
    sentences += chunker.finish();
    for (const QString &sentence : sentences) say(sentence);
}

void VoicePipeline::submitRecording(const QByteArray &pcm16k) {
    cancel();
    if (!recognizer_) {
        emit notice(tr("Speech recognition is not set up — see Settings → Voice."), true);
        return;
    }
    transcribe(pcm16k);
}

void VoicePipeline::onAudio(const QByteArray &pcm) {
    const QList<EnergyVad::Event> events = vad_.feed(pcm);
    emit levelChanged(std::clamp((vad_.levelDb() + 60.0) / 50.0, 0.0, 1.0));
    const bool started = events.contains(EnergyVad::Event::SpeechStarted);
    const bool ended = events.contains(EnergyVad::Event::SpeechEnded);

    if (state_ == VoiceState::Listening && !fromVad_) {
        recording_.append(pcm);
        if (!hold_ && ended) finishListening();     // toggle mode: the pause ends the request
        return;
    }
    if (state_ == VoiceState::Speaking && settings_.bargeIn && started) {
        // Talking over the reply: stop speaking and take what is said as the next request.
        if (speech_) speech_->stop();
        chunker_.reset();
        speakReply_ = false;
        fromVad_ = true;
        hold_ = false;
        listenClock_.restart();
        setState(VoiceState::Listening);
        listenTimer_.start();
        if (replyActive_) emit interruptRequested();
        return;
    }
    if (!ended) return;

    const QByteArray utterance = vad_.takeUtterance();
    if (state_ == VoiceState::Listening && fromVad_) {
        if (awaitingCommand_) {
            checkWakeWord(utterance);       // the command after a bare "Hey Nexa", transcribed locally
        } else {
            recording_ = utterance;
            finishListening();
        }
        return;
    }
    if (wakeWordActive() && state_ != VoiceState::Listening && state_ != VoiceState::Transcribing) {
        checkWakeWord(utterance);
    }
}

void VoicePipeline::finishListening() {
    if (state_ != VoiceState::Listening) return;
    listenTimer_.stop();
    const QByteArray audio = std::exchange(recording_, {});
    const bool heard = fromVad_ || EnergyVad::containsSpeech(audio, vadSettings());
    hold_ = false;
    fromVad_ = false;
    if (!heard || audio.size() < kMinUtteranceBytes) {
        setState(VoiceState::Idle);
        emit notice(tr("I didn't hear anything."), false);
        return;
    }
    transcribe(EnergyVad::trimSilence(audio, vadSettings()));
}

void VoicePipeline::transcribe(const QByteArray &pcm) {
    setState(VoiceState::Transcribing);
    const quint64 generation = ++generation_;
    recognizer_->transcribe(pcm, wake_.recognizerPrompt(), [this, generation](Result<QString> text) {
        if (generation != generation_) return;
        if (!text) {
            setState(VoiceState::Idle);
            if (text.error().code != ErrorCode::Cancelled) {
                emit notice(tr("Speech recognition failed: %1").arg(text.error().message), true);
            }
            return;
        }
        QString heard = cleanTranscript(text.value());
        // "Hey Nexa, open Firefox" said while holding the key: the name is not part of the request.
        if (const QString command = wake_.commandOf(heard); !command.isEmpty()) heard = command;
        if (heard.isEmpty()) {
            setState(VoiceState::Idle);
            emit notice(tr("I didn't catch that."), false);
            return;
        }
        submit(heard);
    });
}

void VoicePipeline::checkWakeWord(const QByteArray &utterance) {
    if (!wakeRecognizer_ || wakeBusy_ || utterance.size() < kMinUtteranceBytes) return;
    wakeBusy_ = true;
    const quint64 generation = generation_;
    wakeRecognizer_->transcribe(utterance, wake_.recognizerPrompt(), [this, generation](Result<QString> text) {
        if (generation != generation_) return;
        wakeBusy_ = false;
        if (!text) return;
        const QString heard = cleanTranscript(text.value());
        if (awaitingCommand_) {
            awaitingCommand_ = false;
            commandTimer_.stop();
            fromVad_ = false;
            if (heard.isEmpty()) {
                setState(VoiceState::Idle);
            } else {
                submit(heard);
            }
            return;
        }
        // Utterances without the wake phrase are dropped unlogged: they were not meant for Nexa.
        const std::optional<WakeWordDetector::Match> wake = wake_.find(heard);
        if (!wake) return;
        if (speech_) speech_->stop();
        if (wake->command.isEmpty()) {
            awaitingCommand_ = true;
            fromVad_ = true;
            listenClock_.restart();
            setState(VoiceState::Listening);
            commandTimer_.start();
            return;
        }
        submit(wake->command);
    });
}

void VoicePipeline::submit(const QString &text) {
    qCDebug(lcVoice) << "request:" << text;
    if (replyActive_) emit interruptRequested();    // a reply still running is replaced
    setState(VoiceState::Thinking);
    emit request(text);
    // Not accepted (no model, a panic word, …) or already over without anything to say.
    if (state_ == VoiceState::Thinking && !replyActive_) setState(VoiceState::Idle);
}

void VoicePipeline::onReplyStarted(const QString &source) {
    replyActive_ = true;
    chunker_.reset();
    spokeReply_ = false;
    const bool fromVoice = source == u"voice"_s;
    speakReply_ = speech_ && (settings_.speak == SpeakMode::Always
                              || (settings_.speak == SpeakMode::VoiceRequests && fromVoice));
}

void VoicePipeline::onReplyDelta(const QString &delta) {
    if (!speakReply_) return;
    for (const QString &sentence : chunker_.feed(delta)) say(sentence);
}

void VoicePipeline::onReplyFinished(FinishReason reason, bool empty) {
    replyActive_ = false;
    if (!std::exchange(speakReply_, false)) {
        if (state_ == VoiceState::Thinking) setState(VoiceState::Idle);
        return;
    }
    if (reason == FinishReason::Cancelled) {
        chunker_.reset();
        if (speech_) speech_->stop();
        if (state_ == VoiceState::Thinking || state_ == VoiceState::Speaking) setState(VoiceState::Idle);
        return;
    }
    for (const QString &sentence : chunker_.finish()) say(sentence);
    // A direct action (no text, or only command output) still gets a spoken confirmation.
    if (!spokeReply_) say(empty ? tr("I got an empty reply.") : tr("Done."));
}

void VoicePipeline::onReplyFailed(const QString &message) {
    Q_UNUSED(message)   // shown in the chat; spoken, it would be long and technical
    replyActive_ = false;
    if (!std::exchange(speakReply_, false)) {
        if (state_ == VoiceState::Thinking) setState(VoiceState::Idle);
        return;
    }
    chunker_.reset();
    say(tr("Sorry, that didn't work. The details are in the chat."));
}

void VoicePipeline::say(const QString &sentence) {
    if (!speech_ || sentence.trimmed().isEmpty()) return;
    speech_->speak(sentence);
    spokeReply_ = true;
    setState(VoiceState::Speaking);
}

void VoicePipeline::onSpeechFinished() {
    if (state_ != VoiceState::Speaking) return;
    // More of the reply may still come: wait for it (thinking), else done.
    setState(replyActive_ && speakReply_ ? VoiceState::Thinking : VoiceState::Idle);
}

void VoicePipeline::setState(VoiceState state) {
    if (state_ == state) return;
    state_ = state;
    updateMicrophone();
    emit stateChanged(state_);
}

void VoicePipeline::updateMicrophone() {
    const bool wanted = state_ == VoiceState::Listening || wakeWordActive()
                        || (settings_.bargeIn && state_ == VoiceState::Speaking);
    if (wanted == input_->isActive()) return;
    if (!wanted) {
        input_->stop();
        emit levelChanged(0.0);
        return;
    }
    vad_.reset();
    if (const Result<void> started = input_->start(); !started) {
        emit notice(started.error().message, true);
        if (state_ == VoiceState::Listening) {
            recording_.clear();
            state_ = VoiceState::Idle;      // directly: setState() would come back here
            emit stateChanged(state_);
        }
    }
}

} // namespace nexa
