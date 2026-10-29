#include "voice/pipeline/VoicePipeline.h"

#include "core/Logging.h"
#include "voice/capture/EnergyVad.h"
#include "voice/capture/IAudioInput.h"
#include "voice/listen/EnergyVoiceDetector.h"
#include "voice/listen/ListeningFront.h"
#include "voice/pipeline/WakeWordDetector.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"
#include "voice/tts/ITextToSpeech.h"
#include "voice/wake/WakePhrases.h"

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kNoSpeechTimeoutMs = 8'000;       // toggle mode: give up when nothing is said
constexpr int kMaxListenMs = 60'000;
constexpr int kCommandTimeoutMs = 8'000;        // after the name alone, for the request to start
constexpr int kTapMs = 350;                     // pressed shorter than this: a tap, not a hold
constexpr qsizetype kMinUtteranceBytes = 16'000 * 2 * 3 / 10;     // 300 ms
constexpr int kWakePauseMs = 700;               // between sentences checked for the name
constexpr int kWakeUtteranceMs = 8'000;         // longer room speech is checked in pieces
constexpr int kWakeRequestMaxMs = 15'000;       // a request after the name: a TV can keep the room from going quiet
constexpr int kCutBeforeWordMs = 200;           // the request starts this long before the name's first word
constexpr qint64 kCutBeforePhrase = 16'000 * 3 / 10;    // …and 0.3 s before the spotter's phrase
constexpr qint64 kBareNameWindow = 16'000 / 2;  // the bare name must open what is being said: within 0.5 s of it
constexpr int kSelfGuardMs = 3'000;             // after Nexa said its own name
constexpr int kFollowUpDelayMs = 150;           // the follow-up window opens this long after the speech
constexpr double kFollowUpQuieterDb = 8.0;      // a follow-up quieter than the request by more is someone else

EnergyVad::Settings vadSettings() {
    EnergyVad::Settings settings;
    settings.hangoverMs = 900;      // a pause this long ends an utterance
    return settings;
}

} // namespace

VoicePipeline::VoicePipeline(IAudioInput *input, QObject *parent)
    : QObject(parent), input_(input) {
    ListeningFront::Parts parts;
    parts.detector = std::make_unique<EnergyVoiceDetector>();
    setListeningFront(std::make_unique<ListeningFront>(std::move(parts), ListeningFront::Threading::Inline));
    connect(input_, &IAudioInput::audio, this, &VoicePipeline::onAudio);
    connect(input_, &IAudioInput::failed, this, [this](const Error &error) {
        emit notice(error.message, true);
        // What was heard so far is cut off: no utterance of it may end later.
        resetHearing();
        capturing_ = false;
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
        if (capturing_) {
            if (elapsed >= kWakeRequestMaxMs) finishCapture(ring_.end());
            return;
        }
        const bool silent = !hold_ && !fromVad_ && !heardSpeech_ && elapsed >= kNoSpeechTimeoutMs;
        if (elapsed >= kMaxListenMs || silent) finishListening();
    });
    followUpOpener_.setSingleShot(true);
    followUpOpener_.setInterval(kFollowUpDelayMs);
    connect(&followUpOpener_, &QTimer::timeout, this, &VoicePipeline::openFollowUp);
    followUpWindow_.setSingleShot(true);
    connect(&followUpWindow_, &QTimer::timeout, this, [this] {
        if (followUp_ && !followUpCounting_) closeFollowUp();      // speech begun in time is heard to its end
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

void VoicePipeline::setListeningFront(std::unique_ptr<ListeningFront> front) {
    if (!front) return;
    front_ = std::move(front);      // the old front goes, with whatever it had not reported yet
    connect(front_.get(), &ListeningFront::speechStarted, this, &VoicePipeline::onSpeechStarted);
    connect(front_.get(), &ListeningFront::speechEnded, this, &VoicePipeline::onSpeechEnded);
    connect(front_.get(), &ListeningFront::wakeHeard, this, &VoicePipeline::onWakeHeard);
    applyPauses();
    ring_.reset();                  // the ring and the front count positions from the same sample
    lastSpeechStart_ = -1;
    capturing_ = false;
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
    applyPauses();
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

QString VoicePipeline::requestPrompt(const QString &wakeName) {
    return u"Hey %1. Ammi ko call karo. Volume kam karo. YouTube par Pehchan OST chalao."_s.arg(wakeName);
}

void VoicePipeline::measureLevels(int ms, std::function<void(MicrophoneCheck::Stats)> done) {
    measuring_ = true;
    measured_.clear();
    updateMicrophone();
    QTimer::singleShot(ms, this, [this, done = std::move(done)] {
        measuring_ = false;
        const MicrophoneCheck::Stats stats = MicrophoneCheck::measure(std::exchange(measured_, {}));
        updateMicrophone();
        done(stats);
    });
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
    capturing_ = false;             // the key wins over a request the name had started
    followUp_ = false;              // …and over the follow-up window
    followUpOpener_.stop();
    followUpWindow_.stop();
    resetHearing();
    heardSpeech_ = false;
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
    capturing_ = false;
    followUp_ = false;
    followUpOpener_.stop();
    followUpWindow_.stop();
    awaitingCommand_ = false;
    wakeBusy_ = false;
    replySaysName_ = false;
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
    const double level = EnergyVad::rmsDb(reinterpret_cast<const qint16 *>(pcm.constData()), pcm.size() / 2);
    emit levelChanged(std::clamp((level + 60.0) / 50.0, 0.0, 1.0));
    micSamples_ += pcm.size() / 2;
    micCheck_.feed(pcm, micSamples_ / 16);
    if (const std::optional<QString> trouble = micCheck_.notice(micSamples_ / 16)) emit microphoneOverloaded(*trouble);
    if (measuring_) measured_.append(pcm);
    ring_.append(pcm);
    if (state_ == VoiceState::Listening && !fromVad_) recording_.append(pcm);
    front_->feed(pcm);      // speech and the name come back through onSpeechStarted/onSpeechEnded
}

void VoicePipeline::onSpeechStarted(qint64 start) {
    lastSpeechStart_ = start;
    heardSpeech_ = true;
    inSpeech_ = true;
    if (followUp_ && !followUpIgnoring_) followUpCounting_ = true;
    if (awaitingCommand_) commandTimer_.stop();     // the request has begun: it ends at the pause
    if (state_ != VoiceState::Speaking || !settings_.bargeIn) return;
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
}

void VoicePipeline::onSpeechEnded(qint64 start, qint64 end) {
    inSpeech_ = false;
    if (capturing_) {
        finishCapture(end);                 // the request after the name ends at the owner's pause
        return;
    }
    if (followUp_) {
        if (std::exchange(followUpIgnoring_, false) || !std::exchange(followUpCounting_, false)) return;
        // Someone across the room — a TV — is quieter than the owner at the laptop.
        if (ring_.levelDb(start, end) < lastRequestLevelDb_ - kFollowUpQuieterDb) {
            if (!followUpWindow_.isActive()) closeFollowUp();
            return;
        }
        followUp_ = false;
        followUpWindow_.stop();
        fromVad_ = false;
        transcribeRequest(ring_.slice(start, end), QString());     // said to Nexa, without the name
        return;
    }
    if (state_ == VoiceState::Listening && !fromVad_) {
        if (!hold_) finishListening();      // toggle mode: the pause ends the request
        return;
    }
    const QByteArray utterance = ring_.slice(start, end);
    if (state_ == VoiceState::Listening && fromVad_) {
        if (awaitingCommand_) {
            // The request after the name alone: said to Nexa, so it goes to the request recogniser.
            awaitingCommand_ = false;
            commandTimer_.stop();
            fromVad_ = false;
            transcribeRequest(utterance, QString());
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

void VoicePipeline::onWakeHeard(const QString &label, qint64 phraseStart, qint64 heardAt) {
    const auto ignored = [&](const char *why) {
        qCDebug(lcVoice).nospace() << "spotter heard " << label << " at " << phraseStart / 16 << " ms: ignored, " << why;
    };
    if (!wakeWordActive() || state_ == VoiceState::Listening || state_ == VoiceState::Transcribing) {
        ignored("busy");
        return;
    }
    // Nexa hearing itself say its name.
    if ((state_ == VoiceState::Speaking && replySaysName_) || (selfGuard_.isValid() && selfGuard_.elapsed() < kSelfGuardMs)) {
        ignored("Nexa's own voice");
        return;
    }
    // The bare name counts where it opens what is being said ("Nexa, …"), not inside a sentence ("I love Nexa").
    if (!WakePhrases::isGreeted(label) && (lastSpeechStart_ < 0 || std::abs(phraseStart - lastSpeechStart_) > kBareNameWindow)) {
        ignored("the bare name inside a sentence");
        return;
    }
    qCDebug(lcVoice).nospace() << "spotter heard " << label << " at " << phraseStart / 16 << " ms (noticed at "
                               << heardAt / 16 << " ms): listening for the request";
    // The second net may be checking the same sentence: this hit is first.
    ++generation_;
    if (wakeRecognizer_) wakeRecognizer_->cancel();
    pendingUtterance_.clear();
    if (speech_) speech_->stop();
    chunker_.reset();
    speakReply_ = false;
    awaitingCommand_ = false;
    commandTimer_.stop();
    // The request: from just before the name — where the spotter is precise — to the pause after it. Nothing
    // said before the name goes with it.
    capturing_ = true;
    captureStart_ = std::max(ring_.begin(), phraseStart - kCutBeforePhrase);
    fromVad_ = true;
    hold_ = false;
    listenClock_.restart();
    setState(VoiceState::Listening);
    applyPauses();
    listenTimer_.start();
    if (replyActive_) emit interruptRequested();
}

void VoicePipeline::finishCapture(qint64 end) {
    capturing_ = false;
    fromVad_ = false;
    listenTimer_.stop();
    transcribeRequest(ring_.slice(captureStart_, end), QString());
}

void VoicePipeline::resetHearing() {
    ring_.reset();
    front_->reset();
    lastSpeechStart_ = -1;
    inSpeech_ = false;
}

void VoicePipeline::maybeOpenFollowUp() {
    if (settings_.followUps && replyFromVoice_ && spokeReply_ && state_ == VoiceState::Idle) followUpOpener_.start();
}

void VoicePipeline::openFollowUp() {
    if (state_ != VoiceState::Idle) return;
    followUp_ = true;
    followUpCounting_ = false;
    fromVad_ = true;
    hold_ = false;
    listenClock_.restart();
    setState(VoiceState::Listening);        // opens the microphone if the wake word had not
    followUpIgnoring_ = inSpeech_;          // speech already going on is not a follow-up
    followUpWindow_.start(settings_.followUpMs);
}

void VoicePipeline::closeFollowUp() {
    followUp_ = false;
    followUpCounting_ = false;
    followUpIgnoring_ = false;
    followUpWindow_.stop();
    fromVad_ = false;
    if (state_ == VoiceState::Listening) setState(VoiceState::Idle);
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
    transcribeRequest(pcm, QString());
}

void VoicePipeline::transcribeRequest(const QByteArray &pcm, const QString &fallback) {
    if (!recognizer_) {
        if (!fallback.isEmpty()) submit(fallback);
        return;
    }
    setState(VoiceState::Transcribing);
    lastRequestLevelDb_ = EnergyVad::rmsDb(reinterpret_cast<const qint16 *>(pcm.constData()), pcm.size() / 2);
    const quint64 generation = ++generation_;
    recognizer_->transcribe(pcm, requestPrompt(settings_.wakeName), [this, generation, fallback](Result<QString> text) {
        if (generation != generation_) return;
        if (!text) {
            if (text.error().code != ErrorCode::Cancelled && !fallback.isEmpty()) {
                submit(fallback);       // the offline recogniser understood it already
                return;
            }
            setState(VoiceState::Idle);
            if (text.error().code != ErrorCode::Cancelled) {
                emit notice(tr("Speech recognition failed: %1").arg(text.error().message), true);
            }
            return;
        }
        const QString heard = cleanTranscript(text.value());
        // Only the name ("Hey Nexa."): the request is still to come.
        if (const auto name = wake_.find(heard); name && name->firstWord == 0 && name->command.isEmpty()) {
            if (fallback.isEmpty()) {
                awaitRequest();
            } else {
                submit(fallback);
            }
            return;
        }
        // "Hey Nexa, open Firefox": the name is not part of the request.
        const QString command = wake_.commandOf(heard);
        if (command.isEmpty()) {
            if (!fallback.isEmpty()) {
                submit(fallback);
                return;
            }
            setState(VoiceState::Idle);
            emit notice(tr("I didn't catch that."), false);
            return;
        }
        submit(command);
    });
}

void VoicePipeline::checkWakeWord(const QByteArray &utterance) {
    if (!wakeRecognizer_ || utterance.size() < kMinUtteranceBytes) return;
    if (wakeBusy_) {
        pendingUtterance_ = utterance;      // checked next; an older one waiting is stale by now
        return;
    }
    wakeBusy_ = true;
    const quint64 generation = generation_;
    wakeRecognizer_->transcribeWords(utterance, wake_.recognizerPrompt(),
                                     [this, generation, utterance](Result<TimedTranscript> transcript) {
        wakeBusy_ = false;
        if (generation != generation_) {
            pendingUtterance_.clear();
            return;
        }
        // Utterances without the name are dropped unlogged: they were not meant for Nexa.
        if (transcript) handleWakeTranscript(utterance, transcript.value());
        if (!pendingUtterance_.isEmpty() && wakeWordActive() && state_ != VoiceState::Listening
            && state_ != VoiceState::Transcribing) {
            checkWakeWord(std::exchange(pendingUtterance_, {}));
        }
    });
}

void VoicePipeline::handleWakeTranscript(const QByteArray &utterance, const TimedTranscript &transcript) {
    const QString heard = cleanTranscript(transcript.text);
    const std::optional<WakeWordDetector::Match> wake = wake_.find(heard);
    if (!wake) return;
    qCDebug(lcVoice) << "second net heard the name in" << heard;
    pendingUtterance_.clear();
    if (speech_) speech_->stop();
    if (wake->command.isEmpty()) {
        awaitRequest();
        return;
    }
    // The request goes to the request recogniser, cut from just before the name — nothing said earlier goes
    // with it. Without word times (or when they do not line up with the words), the offline text is used.
    const QList<TimedWord> &words = transcript.words;
    const qsizetype wordCount = heard.split(u' ', Qt::SkipEmptyParts).size();
    if (words.size() == wordCount && wake->firstWord < words.size()) {
        const int cutMs = std::max(0, words[wake->firstWord].startMs - kCutBeforeWordMs);
        transcribeRequest(utterance.mid(qsizetype(cutMs) * 32), wake->command);
        return;
    }
    submit(wake->command);
}

void VoicePipeline::awaitRequest() {
    if (speech_) speech_->stop();
    awaitingCommand_ = true;
    fromVad_ = true;
    hold_ = false;
    heardSpeech_ = false;
    listenClock_.restart();
    setState(VoiceState::Listening);
    applyPauses();          // fromVad_ may have changed while the state did not
    commandTimer_.start();
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
    replyFromVoice_ = fromVoice;
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
    // Everything was said already: the reply is over.
    if (state_ == VoiceState::Thinking) {
        setState(VoiceState::Idle);
        maybeOpenFollowUp();
    }
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
    if (wake_.mentions(sentence)) replySaysName_ = true;    // the spotter will hear it: not the owner
    speech_->speak(sentence);
    spokeReply_ = true;
    setState(VoiceState::Speaking);
}

void VoicePipeline::onSpeechFinished() {
    if (state_ != VoiceState::Speaking) return;
    if (std::exchange(replySaysName_, false)) selfGuard_.restart();     // its echo may still be heard
    // More of the reply may still come: wait for it (thinking), else done — and listen for a follow-up.
    const bool more = replyActive_ && speakReply_;
    setState(more ? VoiceState::Thinking : VoiceState::Idle);
    if (!more) maybeOpenFollowUp();
}

void VoicePipeline::setState(VoiceState state) {
    if (state_ == state) return;
    state_ = state;
    applyPauses();
    updateMicrophone();
    emit stateChanged(state_);
}

void VoicePipeline::applyPauses() {
    // A request ends at the owner's pause (push-to-talk is capped by its own timer); room speech is cut into
    // sentences for the name.
    const bool request = state_ == VoiceState::Listening;
    front_->setEndPauseMs(request ? settings_.endPauseMs : kWakePauseMs);
    front_->setMaxUtteranceMs(!request ? kWakeUtteranceMs : fromVad_ ? kWakeRequestMaxMs : kMaxListenMs);
}

void VoicePipeline::updateMicrophone() {
    const bool wanted = state_ == VoiceState::Listening || wakeWordActive() || measuring_
                        || (settings_.bargeIn && state_ == VoiceState::Speaking);
    if (wanted == input_->isActive()) return;
    if (!wanted) {
        input_->stop();
        emit levelChanged(0.0);
        return;
    }
    resetHearing();
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
