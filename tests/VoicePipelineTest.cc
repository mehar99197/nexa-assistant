// VoicePipeline driven with a fake microphone, recogniser and voice: push-to-talk, the pause that
// ends a request, speaking replies sentence by sentence, barge-in, the wake word and failures.

#include "voice/capture/IAudioInput.h"
#include "voice/capture/EnergyVad.h"
#include "voice/listen/EnergyVoiceDetector.h"
#include "voice/listen/IAudioFilter.h"
#include "voice/listen/ListeningFront.h"
#include "voice/pipeline/VoicePipeline.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/wake/IWakeSpotter.h"
#include "voice/tts/ITextToSpeech.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QSignalSpy>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

class FakeMicrophone : public IAudioInput {
public:
    QString name() const override { return u"fake"_s; }
    Result<void> start() override {
        if (broken) return Error{ErrorCode::Io, u"no microphone"_s};
        active = true;
        ++starts;
        return {};
    }
    void stop() override { active = false; }
    bool isActive() const override { return active; }

    void play(const QByteArray &pcm) {
        for (qsizetype offset = 0; offset < pcm.size(); offset += 640) emit audio(pcm.mid(offset, 640));
    }

    bool active{false};
    bool broken{false};
    int starts{0};
};

class FakeRecognizer : public ISpeechRecognizer {
public:
    explicit FakeRecognizer(bool local) : local_(local) {}
    QString name() const override { return local_ ? u"local"_s : u"cloud"_s; }
    bool isLocal() const override { return local_; }
    void transcribe(const QByteArray &pcm16k, const QString &prompt, Done done) override {
        audio = pcm16k;
        lastPrompt = prompt;
        ++requests;
        done_ = std::move(done);
    }
    void transcribeWords(const QByteArray &pcm16k, const QString &prompt, DoneWords done) override {
        audio = pcm16k;
        lastPrompt = prompt;
        ++requests;
        doneWords_ = std::move(done);
    }
    void cancel() override {
        if (done_) std::exchange(done_, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
        if (doneWords_) std::exchange(doneWords_, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }
    bool pending() const { return bool(done_) || bool(doneWords_); }
    void answer(Result<QString> text) {
        if (doneWords_) {
            if (!text) {
                std::exchange(doneWords_, nullptr)(text.error());
            } else {
                std::exchange(doneWords_, nullptr)(TimedTranscript{.text = text.value(), .words = {}});
            }
            return;
        }
        std::exchange(done_, nullptr)(std::move(text));
    }
    // With the time of each word, as whisper.cpp gives them.
    void answerWords(const QString &text, const QList<TimedWord> &words) {
        std::exchange(doneWords_, nullptr)(TimedTranscript{.text = text, .words = words});
    }

    QByteArray audio;
    QString lastPrompt;
    int requests{0};

private:
    bool local_;
    Done done_;
    DoneWords doneWords_;
};

class FakeVoice : public ITextToSpeech {
public:
    QString name() const override { return u"fake voice"_s; }
    void speak(const QString &text) override {
        spoken.append(text);
        speaking = true;
    }
    void stop() override {
        speaking = false;
        ++stops;
    }
    bool isSpeaking() const override { return speaking; }
    void finishSpeaking() {
        speaking = false;
        emit finished();
    }

    QStringList spoken;
    bool speaking{false};
    int stops{0};
};

struct Rig {
    FakeMicrophone microphone;
    VoicePipeline pipeline{&microphone};
    FakeRecognizer *recognizer{nullptr};
    FakeVoice *voice{nullptr};
    QStringList requests;
    int interrupts{0};

    Rig() {
        auto ownedRecognizer = std::make_unique<FakeRecognizer>(false);
        recognizer = ownedRecognizer.get();
        pipeline.setRecognizer(std::move(ownedRecognizer));
        auto ownedVoice = std::make_unique<FakeVoice>();
        voice = ownedVoice.get();
        pipeline.setTextToSpeech(std::move(ownedVoice));
        QObject::connect(&pipeline, &VoicePipeline::request, &pipeline, [this](const QString &text) { requests.append(text); });
        QObject::connect(&pipeline, &VoicePipeline::interruptRequested, &pipeline, [this] { ++interrupts; });
    }

    // What AppController does with a request: a reply that streams `deltas`.
    void reply(const QStringList &deltas, FinishReason reason = FinishReason::Stop) {
        pipeline.onReplyStarted(u"voice"_s);
        for (const QString &delta : deltas) pipeline.onReplyDelta(delta);
        pipeline.onReplyFinished(reason, deltas.isEmpty());
    }
};

} // namespace

TEST_CASE("Push-to-talk: hold, speak, release → request → spoken reply") {
    Rig rig;
    rig.pipeline.startListening(true);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    CHECK(rig.microphone.active);

    // Held: a pause does not end the request.
    rig.microphone.play(testaudio::quiet(300) + testaudio::speech(600) + testaudio::quiet(1500));
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.pipeline.stopListening();
    CHECK(rig.pipeline.state() == VoiceState::Transcribing);
    CHECK_FALSE(rig.microphone.active);
    // Trimmed for the recogniser: 300 ms before the speech, 300 ms after.
    CHECK(rig.recognizer->audio.size() == (300 + 600 + 300) * 32);

    rig.recognizer->answer(u" Hey Nexa, what's the weather? "_s);
    CHECK(rig.requests == QStringList{u"what's the weather?"_s});
    CHECK(rig.pipeline.state() == VoiceState::Idle);    // no reply arrived (the fake controller ignored it)

    rig.reply({u"It is sunny. Twenty"_s, u" degrees today."_s});
    CHECK(rig.voice->spoken == QStringList{u"It is sunny."_s, u"Twenty degrees today."_s});
    CHECK(rig.pipeline.state() == VoiceState::Speaking);
    rig.voice->finishSpeaking();
    CHECK(rig.pipeline.state() == VoiceState::Idle);
}

TEST_CASE("Releasing the key: a hold ends the request, a tap keeps listening") {
    Rig tapped;
    tapped.pipeline.startListening(true);
    tapped.pipeline.releaseHold();                      // let go at once: a tap
    CHECK(tapped.pipeline.state() == VoiceState::Listening);
    tapped.microphone.play(testaudio::quiet(400) + testaudio::speech(600) + testaudio::quiet(1600));
    CHECK(tapped.pipeline.state() == VoiceState::Transcribing);      // the pause (1.4 s) ended it
    CHECK(tapped.recognizer->requests == 1);

    Rig held;
    held.pipeline.startListening(true);
    held.microphone.play(testaudio::quiet(300) + testaudio::speech(500) + testaudio::quiet(1200));
    CHECK(held.pipeline.state() == VoiceState::Listening);           // held: the pause is ignored
    QTest::qWait(400);
    held.pipeline.releaseHold();
    CHECK(held.pipeline.state() == VoiceState::Transcribing);
    CHECK(held.recognizer->requests == 1);
}

TEST_CASE("Toggle mode ends the request at the pause; silence ends it unheard") {
    Rig rig;
    QSignalSpy notices(&rig.pipeline, &VoicePipeline::notice);
    rig.pipeline.toggleListening();
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(800) + testaudio::quiet(1600));
    CHECK(rig.pipeline.state() == VoiceState::Transcribing);
    CHECK(rig.recognizer->requests == 1);
    rig.recognizer->answer(u"[BLANK_AUDIO]"_s);
    CHECK(rig.requests.isEmpty());
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    REQUIRE(notices.size() == 1);
    CHECK(notices.at(0).at(0).toString().contains(u"catch"_s));

    rig.pipeline.toggleListening();
    rig.microphone.play(testaudio::quiet(1000));
    rig.pipeline.toggleListening();     // clicked again: nothing was said
    CHECK(rig.recognizer->requests == 1);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    CHECK(notices.size() == 2);
}

TEST_CASE("A pause shorter than the setting does not end the request") {
    const QByteArray said = testaudio::quiet(300) + testaudio::speech(600) + testaudio::quiet(1200)
                          + testaudio::speech(600) + testaudio::quiet(1600);
    Rig normal;
    normal.pipeline.setSettings({.endPauseMs = 1400});
    normal.pipeline.startListening(false);      // a tap: hands-free until the pause
    normal.microphone.play(said);
    CHECK(normal.pipeline.state() == VoiceState::Transcribing);
    CHECK(normal.recognizer->requests == 1);
    CHECK(normal.recognizer->audio.size() / 32 >= 2400);    // both stretches of speech

    Rig quick;
    quick.pipeline.setSettings({.endPauseMs = 900});
    quick.pipeline.startListening(false);
    quick.microphone.play(said);
    CHECK(quick.recognizer->requests == 1);
    CHECK(quick.recognizer->audio.size() / 32 < 1400);      // only the first
}

TEST_CASE("A direct action is confirmed aloud; typed requests stay silent unless asked") {
    Rig rig;
    rig.pipeline.onReplyStarted(u"voice"_s);
    rig.pipeline.onReplyDelta(u"```text\nexit 0\n```"_s);       // command output only
    rig.pipeline.onReplyFinished(FinishReason::Stop, false);
    CHECK(rig.voice->spoken == QStringList{u"Done."_s});

    rig.voice->spoken.clear();
    rig.pipeline.onReplyStarted(u"gui"_s);
    rig.pipeline.onReplyDelta(u"Typed answer."_s);
    rig.pipeline.onReplyFinished(FinishReason::Stop, false);
    CHECK(rig.voice->spoken.isEmpty());

    rig.pipeline.setSettings({.speak = VoicePipeline::SpeakMode::Always});
    rig.pipeline.onReplyStarted(u"gui"_s);
    rig.pipeline.onReplyDelta(u"Typed answer."_s);
    rig.pipeline.onReplyFinished(FinishReason::Stop, false);
    CHECK(rig.voice->spoken == QStringList{u"Typed answer."_s});

    rig.voice->spoken.clear();
    rig.pipeline.onReplyStarted(u"voice"_s);
    rig.pipeline.onReplyFailed(u"HTTP 500"_s);
    REQUIRE(rig.voice->spoken.size() == 1);
    CHECK(rig.voice->spoken.first().startsWith(u"Sorry"_s));
}

TEST_CASE("Pressing the key while Nexa talks cuts the speech and replaces the reply") {
    Rig rig;
    rig.pipeline.onReplyStarted(u"voice"_s);
    rig.pipeline.onReplyDelta(u"A long answer starts here. And"_s);
    CHECK(rig.pipeline.state() == VoiceState::Speaking);

    rig.pipeline.startListening(true);
    CHECK(rig.voice->stops >= 1);
    CHECK_FALSE(rig.voice->speaking);
    CHECK(rig.interrupts == 1);
    CHECK(rig.pipeline.state() == VoiceState::Listening);

    // The cancelled reply's end must not be spoken over the new request.
    rig.pipeline.onReplyFinished(FinishReason::Cancelled, false);
    CHECK(rig.voice->spoken == QStringList{u"A long answer starts here."_s});
    CHECK(rig.pipeline.state() == VoiceState::Listening);
}

TEST_CASE("Barge-in by voice stops the speech when enabled") {
    Rig rig;
    rig.pipeline.setSettings({.bargeIn = true});
    rig.pipeline.speak(u"Here is a long story."_s);
    CHECK(rig.pipeline.state() == VoiceState::Speaking);
    CHECK(rig.microphone.active);       // listening for the user while speaking

    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(700));
    CHECK_FALSE(rig.voice->speaking);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::quiet(1500));
    CHECK(rig.pipeline.state() == VoiceState::Transcribing);
    rig.recognizer->answer(u"Stop."_s);
    CHECK(rig.requests == QStringList{u"Stop."_s});
}

TEST_CASE("Cancel drops the utterance, the transcription and the speech") {
    Rig rig;
    rig.pipeline.startListening(true);
    rig.microphone.play(testaudio::speech(500));
    rig.pipeline.stopListening();
    REQUIRE(rig.recognizer->pending());
    rig.pipeline.cancel();
    CHECK_FALSE(rig.recognizer->pending());
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    CHECK(rig.requests.isEmpty());
    CHECK_FALSE(rig.microphone.active);
}

TEST_CASE("Wake word: only utterances with the name become requests") {
    Rig rig;
    auto wake = std::make_unique<FakeRecognizer>(true);
    FakeRecognizer *local = wake.get();
    rig.pipeline.setWakeRecognizer(std::move(wake));
    CHECK_FALSE(rig.microphone.active);         // off until enabled
    rig.pipeline.setSettings({.wakeWord = true});
    CHECK(rig.pipeline.wakeWordActive());
    CHECK(rig.microphone.active);

    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(700) + testaudio::quiet(1200));
    REQUIRE(local->pending());
    local->answer(u"So I said to him, no way."_s);
    CHECK(rig.requests.isEmpty());
    CHECK(rig.recognizer->requests == 0);      // room audio never reaches the cloud recogniser

    rig.microphone.play(testaudio::speech(700) + testaudio::quiet(1200));
    local->answer(u"Hey Nexa, pause the music."_s);
    CHECK(rig.requests == QStringList{u"pause the music."_s});

    // The name alone: the next utterance is the request — said to Nexa, so the request recogniser hears it.
    rig.microphone.play(testaudio::speech(500) + testaudio::quiet(1200));
    local->answer(u"Hey Nexa."_s);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::speech(900) + testaudio::quiet(1600));     // a request: the 1.4 s pause
    REQUIRE(rig.recognizer->pending());
    rig.recognizer->answer(u"Open the calculator."_s);
    CHECK(rig.requests.last() == u"Open the calculator."_s);

    rig.pipeline.setSettings({.wakeWord = false});
    CHECK_FALSE(rig.microphone.active);
}

namespace {

// Halves every sample: what the detectors hear is quieter than what was said.
class HalvingFilter : public IAudioFilter {
public:
    QByteArray process(QByteArrayView pcm16k) override {
        QByteArray out(pcm16k.data(), pcm16k.size());
        auto *samples = reinterpret_cast<qint16 *>(out.data());
        for (qsizetype i = 0; i < out.size() / 2; ++i) samples[i] = qint16(samples[i] / 2);
        heard += out.size() / 2;
        return out;
    }
    void reset() override {}
    qint64 heard{0};
};

double levelOf(const QByteArray &pcm) {
    return EnergyVad::rmsDb(reinterpret_cast<const qint16 *>(pcm.constData()), pcm.size() / 2);
}

} // namespace

TEST_CASE("The recogniser gets the raw audio while the detectors hear it filtered") {
    Rig rig;
    auto filter = std::make_unique<HalvingFilter>();
    HalvingFilter *halving = filter.get();
    ListeningFront::Parts parts;
    parts.detector = std::make_unique<EnergyVoiceDetector>();
    parts.filter = std::move(filter);
    rig.pipeline.setListeningFront(std::make_unique<ListeningFront>(std::move(parts), ListeningFront::Threading::Inline));
    auto wake = std::make_unique<FakeRecognizer>(true);
    FakeRecognizer *local = wake.get();
    rig.pipeline.setWakeRecognizer(std::move(wake));
    rig.pipeline.setSettings({.wakeWord = true});

    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(700) + testaudio::quiet(1200));
    REQUIRE(local->pending());
    CHECK(halving->heard > 0);
    // testaudio::speech is about -15 dBFS; halved it would be about -21.
    CHECK(levelOf(local->audio) > -18.0);
}

TEST_CASE("A failing microphone leaves the pipeline idle and the front reset") {
    Rig rig;
    auto wake = std::make_unique<FakeRecognizer>(true);
    FakeRecognizer *local = wake.get();
    rig.pipeline.setWakeRecognizer(std::move(wake));
    rig.pipeline.setSettings({.wakeWord = true});
    QSignalSpy notices(&rig.pipeline, &VoicePipeline::notice);

    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(700));     // an utterance in progress
    emit rig.microphone.failed(Error{ErrorCode::Io, u"The microphone was unplugged."_s});
    CHECK(notices.count() == 1);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    rig.microphone.play(testaudio::quiet(1200));
    CHECK(local->requests == 0);    // the cut-off utterance is not checked for the name
}

namespace {

// A rig listening for the name, with the offline recogniser the second net uses.
struct WakeRig : Rig {
    FakeRecognizer *local{nullptr};
    WakeRig() {
        auto wake = std::make_unique<FakeRecognizer>(true);
        local = wake.get();
        pipeline.setWakeRecognizer(std::move(wake));
        pipeline.setSettings({.wakeWord = true});
    }
};

const QList<TimedWord> kMidWords{{u"I"_s, 0, 300},        {u"was"_s, 300, 500},    {u"just"_s, 500, 800},
                                 {u"saying"_s, 800, 1300}, {u"that,"_s, 1300, 2000}, {u"hey"_s, 2000, 2200},
                                 {u"Nexa,"_s, 2200, 2600}, {u"pause"_s, 2600, 2900}, {u"the"_s, 2900, 3000},
                                 {u"music."_s, 3000, 3300}};
const QString kMidText = u"I was just saying that, hey Nexa, pause the music."_s;

} // namespace

TEST_CASE("Second net: the name mid-utterance → the request from the word time") {
    WakeRig rig;
    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(3000) + testaudio::quiet(1200));
    REQUIRE(rig.local->pending());
    const QByteArray utterance = rig.local->audio;
    rig.local->answerWords(kMidText, kMidWords);
    REQUIRE(rig.recognizer->pending());
    CHECK(rig.recognizer->audio == utterance.mid(1800 * 32));      // from 0.2 s before "hey"
    CHECK(rig.recognizer->lastPrompt == VoicePipeline::requestPrompt(u"Nexa"_s));
    CHECK(rig.pipeline.state() == VoiceState::Transcribing);
    rig.recognizer->answer(u"Hey Nexa, pause the music."_s);
    CHECK(rig.requests == QStringList{u"pause the music."_s});
}

TEST_CASE("Second net: a failing request recogniser → the offline text after the name") {
    WakeRig rig;
    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(3000) + testaudio::quiet(1200));
    rig.local->answerWords(kMidText, kMidWords);
    REQUIRE(rig.recognizer->pending());
    rig.recognizer->answer(Error{ErrorCode::Network, u"offline"_s});
    CHECK(rig.requests == QStringList{u"pause the music."_s});
}

TEST_CASE("An utterance arriving while whisper is busy is checked afterwards, not dropped") {
    WakeRig rig;
    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(700) + testaudio::quiet(1200));
    REQUIRE(rig.local->requests == 1);
    rig.microphone.play(testaudio::speech(700) + testaudio::quiet(1200));    // waits
    rig.microphone.play(testaudio::speech(500) + testaudio::quiet(1200));    // newer: replaces the waiting one
    CHECK(rig.local->requests == 1);
    rig.local->answer(u"So I said to him, no way."_s);
    REQUIRE(rig.local->requests == 2);
    CHECK(rig.local->audio.size() / 32 < 1200);     // the newest one (500 ms of speech), not the 700 ms one
    rig.local->answer(u"Hey Nexa, open files."_s);
    CHECK(rig.requests == QStringList{u"open files."_s});
}

TEST_CASE("The name alone: Nexa listens for the request") {
    WakeRig rig;
    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(500) + testaudio::quiet(1200));
    rig.local->answer(u"Hey Nexa."_s);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::speech(900) + testaudio::quiet(1600));
    REQUIRE(rig.recognizer->pending());
    CHECK(rig.local->requests == 1);       // the request is not the offline recogniser's
    rig.recognizer->answer(u"Open the calculator."_s);
    CHECK(rig.requests == QStringList{u"Open the calculator."_s});
}

TEST_CASE("Only the name back from the recogniser: Nexa waits for the request") {
    Rig rig;
    QSignalSpy notices(&rig.pipeline, &VoicePipeline::notice);
    rig.pipeline.startListening(true);
    rig.microphone.play(testaudio::quiet(300) + testaudio::speech(600) + testaudio::quiet(300));
    rig.pipeline.stopListening();
    rig.recognizer->answer(u"Hey Nexa."_s);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    CHECK(notices.isEmpty());
    CHECK(rig.requests.isEmpty());
    // The microphone opens again for the request; the room is heard before the speech.
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(900) + testaudio::quiet(1600));
    REQUIRE(rig.recognizer->pending());
    rig.recognizer->answer(u"What time is it?"_s);
    CHECK(rig.requests == QStringList{u"What time is it?"_s});
}

TEST_CASE("A request started by the name ends at 15 s when the room never goes quiet") {
    WakeRig rig;
    rig.microphone.play(testaudio::quiet(800) + testaudio::speech(500) + testaudio::quiet(1200));
    rig.local->answer(u"Hey Nexa."_s);
    // Speech that never pauses for long: 400 ms of words, 100 ms between them, for 16.5 s.
    QByteArray endless;
    for (int i = 0; i < 33; ++i) endless += testaudio::speech(400, std::uint32_t(i + 3)) + testaudio::quiet(100);
    rig.microphone.play(endless);
    REQUIRE(rig.recognizer->pending());
    const qint64 ms = rig.recognizer->audio.size() / 32;
    CHECK(ms >= 14800);
    CHECK(ms <= 15600);
}

namespace {

// Hears a phrase that began at `hitAt` (a sample position), once — half a second later, as the real spotter
// notices the name only after it has been said.
class ScriptedSpotter : public IWakeSpotter {
public:
    ScriptedSpotter(QString label, qint64 hitAt) : label_(std::move(label)), hitAt_(hitAt) {}
    QList<Hit> feed(QByteArrayView pcm16k) override {
        seen_ += pcm16k.size() / 2;
        if (fired_ || seen_ <= hitAt_ + 8000) return {};
        fired_ = true;
        return {Hit{.label = label_, .phraseStart = hitAt_, .heardAt = seen_}};
    }
    void reset() override {
        seen_ = 0;
        fired_ = false;
    }

private:
    QString label_;
    qint64 hitAt_;
    qint64 seen_{0};
    bool fired_{false};
};

std::unique_ptr<ListeningFront> spottingFront(const QString &label, qint64 hitAt,
                                              ListeningFront::Threading threading = ListeningFront::Threading::Inline) {
    ListeningFront::Parts parts;
    parts.detector = std::make_unique<EnergyVoiceDetector>();
    parts.spotter = std::make_unique<ScriptedSpotter>(label, hitAt);
    return std::make_unique<ListeningFront>(std::move(parts), threading);
}

// A rig listening for the name with a spotter that hears it at `hitAt`.
struct SpotterRig : WakeRig {
    SpotterRig(const QString &label, qint64 hitAt) { pipeline.setListeningFront(spottingFront(label, hitAt)); }
};

constexpr qint64 kSecond = 16'000;

// Speech as people talk: words of 400 ms with 100 ms between them (a gapless tone becomes the energy
// detector's idea of the room after 1.5 s).
QByteArray talking(int ms) {
    QByteArray pcm;
    for (int at = 0; at < ms; at += 500) pcm += testaudio::speech(400, std::uint32_t(at / 500 + 7)) + testaudio::quiet(100);
    return pcm;
}

} // namespace

TEST_CASE("A spotter hit mid-speech: Listening at once; the request from 0.3 s before the phrase") {
    SpotterRig rig(u"HEY_NEXA"_s, 2 * kSecond);
    rig.microphone.play(testaudio::quiet(1000) + talking(2000));
    CHECK(rig.pipeline.state() == VoiceState::Listening);      // before the speech has ended
    rig.microphone.play(talking(1000) + testaudio::quiet(1600));
    REQUIRE(rig.recognizer->pending());
    // From 1.7 s to the end of the speech (3.9 s) plus the 200 ms kept after it.
    CHECK(std::abs(rig.recognizer->audio.size() / 2 - (41 * kSecond / 10 - 17 * kSecond / 10)) <= 320);
    CHECK(rig.local->requests == 0);       // the sentence with the name is not the second net's
    rig.recognizer->answer(u"Hey Nexa, open YouTube"_s);
    CHECK(rig.requests == QStringList{u"open YouTube"_s});
}

TEST_CASE("A bare-name hit counts only when the name opens the utterance") {
    SpotterRig inside(u"NEXA"_s, 25 * kSecond / 10);         // 1.5 s into what is being said
    inside.microphone.play(testaudio::quiet(1000) + testaudio::speech(3000));
    CHECK(inside.pipeline.state() == VoiceState::Idle);

    SpotterRig opening(u"NEXA"_s, 11 * kSecond / 10);         // as the speech begins
    opening.microphone.play(testaudio::quiet(1000) + testaudio::speech(1500));
    CHECK(opening.pipeline.state() == VoiceState::Listening);
}

TEST_CASE("A hit while speaking stops the speech; Nexa's own name in its speech is ignored") {
    SpotterRig rig(u"HEY_NEXA"_s, 15 * kSecond / 10);
    rig.pipeline.onReplyStarted(u"voice"_s);
    rig.pipeline.onReplyDelta(u"The weather is fine today. It is"_s);
    REQUIRE(rig.pipeline.state() == VoiceState::Speaking);
    rig.microphone.play(testaudio::quiet(1000) + testaudio::speech(1200));
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    CHECK(rig.voice->stops >= 1);
    CHECK(rig.interrupts == 1);

    SpotterRig own(u"HEY_NEXA"_s, 15 * kSecond / 10);
    own.pipeline.onReplyStarted(u"voice"_s);
    own.pipeline.onReplyDelta(u"I am Nexa, your assistant. How"_s);
    REQUIRE(own.pipeline.state() == VoiceState::Speaking);
    own.microphone.play(testaudio::quiet(1000) + testaudio::speech(1200));
    CHECK(own.pipeline.state() == VoiceState::Speaking);
}

TEST_CASE("A hit from a replaced listening front is ignored") {
    WakeRig rig;
    rig.pipeline.setListeningFront(spottingFront(u"HEY_NEXA"_s, kSecond / 2, ListeningFront::Threading::Worker));
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(1000));
    // The name is chosen again before the old front's hit is delivered.
    ListeningFront::Parts parts;
    parts.detector = std::make_unique<EnergyVoiceDetector>();
    rig.pipeline.setListeningFront(std::make_unique<ListeningFront>(std::move(parts), ListeningFront::Threading::Inline));
    QTest::qWait(300);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
}

TEST_CASE("Talk while capturing a request after the name: one request, from the key") {
    SpotterRig rig(u"HEY_NEXA"_s, 15 * kSecond / 10);
    rig.microphone.play(testaudio::quiet(1000) + testaudio::speech(1200));
    REQUIRE(rig.pipeline.state() == VoiceState::Listening);
    rig.pipeline.startListening(true);
    rig.microphone.play(testaudio::quiet(300) + testaudio::speech(600) + testaudio::quiet(300));
    rig.pipeline.stopListening();
    CHECK(rig.recognizer->requests == 1);
    rig.recognizer->answer(u"open files"_s);
    rig.microphone.play(testaudio::quiet(1600));
    CHECK(rig.recognizer->requests == 1);
    CHECK(rig.requests == QStringList{u"open files"_s});
}

namespace {

// A spoken request answered aloud — what opens the follow-up window.
void askAndHear(Rig &rig) {
    rig.pipeline.startListening(false);
    rig.microphone.play(testaudio::quiet(400) + testaudio::speech(800) + testaudio::quiet(1600));
    REQUIRE(rig.recognizer->pending());
    rig.recognizer->answer(u"What's the weather?"_s);
    rig.reply({u"It is sunny. Twenty"_s, u" degrees today."_s});
    rig.voice->finishSpeaking();
}

} // namespace

TEST_CASE("A follow-up in the window after the spoken reply is a request without the name") {
    Rig rig;
    rig.pipeline.setSettings({.followUpMs = 2000});
    askAndHear(rig);
    QTest::qWait(250);      // the window opens 150 ms after the speech
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(800) + testaudio::quiet(1600));
    REQUIRE(rig.recognizer->pending());
    rig.recognizer->answer(u"And tomorrow?"_s);
    CHECK(rig.requests == QStringList{u"What's the weather?"_s, u"And tomorrow?"_s});
}

TEST_CASE("Silence after the reply ends the window") {
    Rig rig;
    rig.pipeline.setSettings({.followUpMs = 300});
    askAndHear(rig);
    QTest::qWait(250);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    QTest::qWait(400);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    CHECK_FALSE(rig.microphone.active);
}

TEST_CASE("No follow-up after a typed request or with the setting off") {
    Rig typed;
    typed.pipeline.setSettings({.speak = VoicePipeline::SpeakMode::Always, .followUpMs = 2000});
    typed.pipeline.onReplyStarted(u"gui"_s);
    typed.pipeline.onReplyDelta(u"Done it. All"_s);
    typed.pipeline.onReplyFinished(FinishReason::Stop, false);
    typed.voice->finishSpeaking();
    QTest::qWait(250);
    CHECK(typed.pipeline.state() == VoiceState::Idle);

    Rig off;
    off.pipeline.setSettings({.followUps = false});
    askAndHear(off);
    QTest::qWait(250);
    CHECK(off.pipeline.state() == VoiceState::Idle);
}

TEST_CASE("Speech already going on, or much quieter than the request, does not count") {
    WakeRig rig;
    rig.pipeline.setSettings({.wakeWord = true, .followUpMs = 1500});
    rig.pipeline.startListening(false);
    rig.microphone.play(testaudio::quiet(400) + testaudio::speech(800) + testaudio::quiet(1600));
    rig.recognizer->answer(u"What's the weather?"_s);
    rig.reply({u"It is sunny. Twenty"_s, u" degrees today."_s});
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(500));       // someone starts talking
    rig.voice->finishSpeaking();
    QTest::qWait(250);
    REQUIRE(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::speech(400) + testaudio::quiet(800));       // …and stops: it began before
    const QByteArray softly = testaudio::make(700, 0.08, 0.006);              // 10 dB below the request
    rig.microphone.play(testaudio::quiet(300) + softly + testaudio::quiet(1600));
    CHECK(rig.recognizer->requests == 1);
    QTest::qWait(1500);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    CHECK(rig.recognizer->requests == 1);
}

TEST_CASE("Talk in the follow-up window: one request, from the key") {
    Rig rig;
    rig.pipeline.setSettings({.followUpMs = 2000});
    askAndHear(rig);
    QTest::qWait(250);
    REQUIRE(rig.pipeline.state() == VoiceState::Listening);
    rig.pipeline.startListening(true);
    rig.microphone.play(testaudio::quiet(300) + testaudio::speech(600) + testaudio::quiet(300));
    rig.pipeline.stopListening();
    CHECK(rig.recognizer->requests == 2);
    rig.recognizer->answer(u"open files"_s);
    rig.microphone.play(testaudio::quiet(1600));
    CHECK(rig.recognizer->requests == 2);
    CHECK(rig.requests == QStringList{u"What's the weather?"_s, u"open files"_s});
}

TEST_CASE("The wake word refuses a cloud recogniser") {
    Rig rig;
    rig.pipeline.setWakeRecognizer(std::make_unique<FakeRecognizer>(false));
    rig.pipeline.setSettings({.wakeWord = true});
    CHECK_FALSE(rig.pipeline.wakeWordActive());
    CHECK_FALSE(rig.microphone.active);
}

TEST_CASE("Failures are reported and leave the pipeline idle") {
    Rig rig;
    QSignalSpy notices(&rig.pipeline, &VoicePipeline::notice);
    rig.pipeline.startListening(true);
    rig.microphone.play(testaudio::speech(500) + testaudio::quiet(200));
    rig.pipeline.stopListening();
    rig.recognizer->answer(Error{ErrorCode::Network, u"the provider is down"_s});
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    REQUIRE(notices.size() == 1);
    CHECK(notices.at(0).at(0).toString().contains(u"the provider is down"_s));
    CHECK(notices.at(0).at(1).toBool());

    rig.microphone.broken = true;
    rig.pipeline.startListening(false);
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    CHECK(notices.size() == 2);

    rig.pipeline.setRecognizer(nullptr);
    rig.pipeline.startListening(false);
    CHECK(notices.size() == 3);
    CHECK_FALSE(rig.pipeline.canListen());
}
