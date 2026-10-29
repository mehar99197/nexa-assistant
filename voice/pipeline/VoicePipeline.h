#pragma once

#include "core/ai/ChatStream.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/listen/AudioRing.h"
#include "voice/pipeline/WakeWordDetector.h"
#include "voice/tts/SentenceChunker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <memory>

namespace nexa {

class IAudioInput;
class ListeningFront;
class ISpeechRecognizer;
class ITextToSpeech;

enum class VoiceState { Idle, Listening, Transcribing, Thinking, Speaking };

// State machine behind voice control: idle → listening → transcribing → thinking → speaking.
// startListening() opens the microphone — held (push-to-talk) until stopListening(), otherwise
// until the speaker pauses. The transcript leaves through request(); the reply comes back through
// the reply slots and is spoken sentence by sentence as it streams in. Pressing the key again cuts
// the speech (and, with barge-in, so does talking). With the wake word on, the microphone stays
// open and only utterances with the wake name ("Hey Nexa") become requests — transcribed by the local
// recogniser only, so room audio never leaves the machine. What it hears goes through a listening front (noise
// reduction, the voice detector, the wake-name spotter); requests are cut, as said, from a ring of raw audio.
class VoicePipeline : public QObject {
    Q_OBJECT
public:
    enum class SpeakMode { VoiceRequests, Always, Never };

    struct Settings {
        SpeakMode speak{SpeakMode::VoiceRequests};
        bool wakeWord{false};
        bool bargeIn{false};
        QString wakeName{QStringLiteral("Nexa")};
        int endPauseMs{1400};       // this much quiet ends a request (Settings → Voice → Pause before I'm done)
    };

    explicit VoicePipeline(IAudioInput *input, QObject *parent = nullptr);
    ~VoicePipeline() override;

    // Backends can be swapped at any time (Settings → Voice); null switches the feature off.
    void setRecognizer(std::unique_ptr<ISpeechRecognizer> recognizer);
    void setWakeRecognizer(std::unique_ptr<ISpeechRecognizer> recognizer);     // must be local
    void setTextToSpeech(std::unique_ptr<ITextToSpeech> speech);
    // Replaces what hears speech and the name (an energy detector, inline, until one is set).
    void setListeningFront(std::unique_ptr<ListeningFront> front);
    void setSettings(Settings settings);

    VoiceState state() const { return state_; }
    bool canListen() const { return recognizer_ != nullptr; }
    bool canSpeak() const { return speech_ != nullptr; }
    bool wakeWordActive() const;
    QString recognizerName() const;
    QString speechName() const;

    static SpeakMode speakModeFromString(const QString &mode);
    // The spelling hint for a request: the name, and Roman-Urdu examples that pull Urdu towards Latin letters.
    static QString requestPrompt(const QString &wakeName);

public slots:
    void toggleListening();                     // mic button, hotkey
    void startListening(bool hold = false);     // hold: until stopListening() (push-to-talk)
    // The key or button held down was let go: a real hold ends the request, a tap keeps
    // listening until the speaker pauses.
    void releaseHold();
    void stopListening();
    void cancel();                              // drop the utterance; stop transcribing and speaking
    void speak(const QString &text);            // the speak tool, Settings → Test
    void submitRecording(const QByteArray &pcm16k);     // a whole utterance recorded elsewhere

    // The reply side, wired to AppController.
    void onReplyStarted(const QString &source);
    void onReplyDelta(const QString &delta);
    void onReplyFinished(nexa::FinishReason reason, bool empty);
    void onReplyFailed(const QString &message);

signals:
    void stateChanged(nexa::VoiceState state);
    void settingsChanged();                     // setSettings() applied (e.g. the wake word went on)
    void levelChanged(double level);            // 0 … 1 while the microphone is open
    void request(const QString &text);          // a recognised request
    void interruptRequested();                  // the user spoke over a reply still being generated
    void notice(const QString &message, bool isError);

private:
    void onAudio(const QByteArray &pcm);
    void onSpeechStarted(qint64 start);
    void onSpeechEnded(qint64 start, qint64 end);
    void finishListening();
    void transcribe(const QByteArray &pcm);
    // A request said to Nexa, through the request recogniser; `fallback` (the offline text after the name) is
    // used when that recogniser fails or hears only the name.
    void transcribeRequest(const QByteArray &pcm, const QString &fallback);
    void checkWakeWord(const QByteArray &utterance);
    void handleWakeTranscript(const QByteArray &utterance, const TimedTranscript &transcript);
    void awaitRequest();            // the name alone was said: listen for the request
    void submit(const QString &text);
    void say(const QString &sentence);
    void onSpeechFinished();
    void setState(VoiceState state);
    void updateMicrophone();
    void applyPauses();             // the front's pause and longest utterance for what is being listened for

    IAudioInput *input_;
    std::unique_ptr<ISpeechRecognizer> recognizer_;
    std::unique_ptr<ISpeechRecognizer> wakeRecognizer_;
    std::unique_ptr<ITextToSpeech> speech_;
    Settings settings_;
    VoiceState state_{VoiceState::Idle};

    AudioRing ring_;
    std::unique_ptr<ListeningFront> front_;
    bool heardSpeech_{false};           // since listening began
    QByteArray recording_;
    bool hold_{false};
    bool fromVad_{false};               // this utterance is the VAD's (barge-in), not a key press
    QElapsedTimer listenClock_;
    QTimer listenTimer_;                // limits of a listening session
    quint64 generation_{0};             // transcriptions of a cancelled utterance are dropped

    WakeWordDetector wake_;
    bool wakeBusy_{false};
    QByteArray pendingUtterance_;       // heard while the second net was busy: the newest one waits
    bool awaitingCommand_{false};       // the wake phrase was said alone: the next utterance is the command
    QTimer commandTimer_;

    SentenceChunker chunker_;
    bool replyActive_{false};
    bool speakReply_{false};
    bool spokeReply_{false};
};

} // namespace nexa
