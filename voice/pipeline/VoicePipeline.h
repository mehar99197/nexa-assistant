#pragma once

#include "core/ai/ChatStream.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/capture/MicrophoneCheck.h"
#include "voice/listen/AudioRing.h"
#include "voice/pipeline/WakeWordDetector.h"
#include "voice/tts/SentenceChunker.h"

#include <QElapsedTimer>
#include <QObject>
#include <QTimer>

#include <functional>
#include <memory>

namespace nexa {

class IAudioInput;
class ISpeakerGate;
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
    // What interrupts the speech: the name (the spotter, which the owner's voice reaches and the speakers' "Nexa" is
    // guarded against), any speech (with headphones), or nothing said.
    enum class BargeIn { Name, Speech, Off };

    struct Settings {
        SpeakMode speak{SpeakMode::VoiceRequests};
        bool wakeWord{false};
        BargeIn bargeIn{BargeIn::Name};
        QString wakeName{QStringLiteral("Nexa")};
        int endPauseMs{1400};       // this much quiet ends a request (Settings → Voice → Pause before I'm done)
        bool followUps{true};       // after a spoken reply, listen for a follow-up without the name
        int followUpMs{8000};       // …for this long
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
    // Whose voice: what is said hands-free (after the name, in a follow-up, over the speech) goes on only when the gate
    // says it is the owner's; push-to-talk never asks. Not owned; null: anyone.
    void setSpeakerGate(ISpeakerGate *gate) { gate_ = gate; }
    // Whether the request just emitted was checked by the gate and found to be the owner's (an enrolled voice, said
    // hands-free); false for Talk, a recording, and any request while no voice is enrolled. Read it in a slot of
    // request(). Hardcore mode lets only the owner's own voice end its lock.
    bool lastRequestFromOwner() const { return lastFromOwner_; }

    VoiceState state() const { return state_; }
    bool canListen() const { return recognizer_ != nullptr; }
    bool canSpeak() const { return speech_ != nullptr; }
    bool wakeWordActive() const;
    QString recognizerName() const;
    QString speechName() const;
    // What is wrong with the microphone's sound (clipped, drowned in noise); empty when it is fine.
    QString microphoneTrouble() const { return micCheck_.describe(); }
    // Keeps the microphone open for `ms` and measures what it hears (Settings' "Lower it").
    void measureLevels(int ms, std::function<void(MicrophoneCheck::Stats)> done);
    // Keeps the microphone open for `ms` and hands back what it heard, as it came (enrolling the owner's voice).
    void recordFor(int ms, std::function<void(QByteArray pcm16k)> done);

    static SpeakMode speakModeFromString(const QString &mode);
    static BargeIn bargeInFromString(const QString &mode);
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
    // "Stop talking": the voice goes quiet, the reply goes on (in the chat) and nothing else is dropped.
    void stopSpeaking();
    // "That's all": the conversation by voice ends — no follow-up, no request awaited after the name.
    void endConversation();
    void speak(const QString &text);            // the speak tool, Settings → Test
    void submitRecording(const QByteArray &pcm16k);     // a whole utterance recorded elsewhere

    // The reply side, wired to AppController.
    void onReplyStarted(const QString &source);
    void onReplyDelta(const QString &delta);
    void onReplyFinished(nexa::FinishReason reason, bool empty);
    void onReplyFailed(const QString &message);

signals:
    // A voice that is not the owner's was turned away (its score, for Settings).
    void strangerIgnored(double score);
    void stateChanged(nexa::VoiceState state);
    void settingsChanged();                     // setSettings() applied (e.g. the wake word went on)
    void levelChanged(double level);            // 0 … 1 while the microphone is open
    void request(const QString &text);          // a recognised request
    void interruptRequested();                  // the user spoke over a reply still being generated
    void notice(const QString &message, bool isError);
    void microphoneOverloaded(const QString &notice);   // at most once a minute while it lasts

private:
    void onAudio(const QByteArray &pcm);
    void onSpeechStarted(qint64 start);
    void onSpeechEnded(qint64 start, qint64 end);
    void onWakeHeard(const QString &label, qint64 phraseStart, qint64 heardAt);
    void finishCapture(qint64 end);     // the request after the spotter's hit, up to `end` in the ring
    void resetHearing();                // the ring and the front start counting again
    void maybeOpenFollowUp();           // after the spoken reply to a spoken request
    void openFollowUp();
    void closeFollowUp();
    void finishListening();
    void transcribe(const QByteArray &pcm);
    // A request said to Nexa, through the request recogniser; `fallback` (the offline text after the name) is
    // used when that recogniser fails or hears only the name. `needsName`: the spotter's capture, which counts only
    // when the words confirm the name (or a greeting) opens it.
    void transcribeRequest(const QByteArray &pcm, const QString &fallback, bool needsName = false);
    void checkWakeWord(const QByteArray &utterance);
    // `then` when it is the owner speaking in `pcm` (or no gate is on); else the utterance is dropped, quietly.
    void fromOwner(const QByteArray &pcm, std::function<void()> then, std::function<void()> otherwise = {});
    void transcribeRoom(const QByteArray &utterance);       // the second net's whisper
    void handleWakeTranscript(const QByteArray &utterance, const TimedTranscript &transcript);
    void awaitRequest();            // the name alone was said: listen for the request
    void submit(const QString &text);
    void say(const QString &sentence);
    void onSpeechFinished();
    void setState(VoiceState state);
    void updateMicrophone();
    void applyPauses();             // the front's pause and longest utterance for what is being listened for

    IAudioInput *input_;
    ISpeakerGate *gate_{nullptr};
    bool ownerHeard_{false};            // the gate said the owner's for the utterance being turned into a request
    bool lastFromOwner_{false};         // …and so it was, for the request last emitted
    std::unique_ptr<ISpeechRecognizer> recognizer_;
    std::unique_ptr<ISpeechRecognizer> wakeRecognizer_;
    std::unique_ptr<ITextToSpeech> speech_;
    Settings settings_;
    VoiceState state_{VoiceState::Idle};

    AudioRing ring_;
    std::unique_ptr<ListeningFront> front_;
    bool heardSpeech_{false};           // since listening began
    qint64 lastSpeechStart_{-1};        // where the voice detector last heard speech begin
    bool capturing_{false};             // a request after the spotter heard the name
    qint64 captureStart_{0};            // its start in the ring: just before the name
    bool replySaysName_{false};         // the speech playing says the name: Nexa would hear itself
    QElapsedTimer selfGuard_;           // since such speech ended
    bool inSpeech_{false};              // the voice detector heard speech begin and not yet end
    bool replyFromVoice_{false};        // the reply answers a spoken request
    bool followUp_{false};              // the window after a spoken reply is open
    bool followUpIgnoring_{false};      // the speech going on when it opened does not count
    bool followUpCounting_{false};      // speech that began in the window: heard to its end
    double lastRequestLevelDb_{-100.0}; // how loud the owner's last request was
    MicrophoneCheck micCheck_;
    qint64 micSamples_{0};              // heard since Nexa started: the check's clock
    bool measuring_{false};
    QByteArray measured_;
    QTimer followUpOpener_;             // the window opens a moment after the speech
    QTimer followUpWindow_;
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
