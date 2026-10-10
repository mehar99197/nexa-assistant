// VoicePipeline driven with a fake microphone, recogniser and voice: push-to-talk, the pause that
// ends a request, speaking replies sentence by sentence, barge-in, the wake word and failures.

#include "voice/capture/IAudioInput.h"
#include "voice/pipeline/VoicePipeline.h"
#include "voice/stt/ISpeechRecognizer.h"
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
    void transcribe(const QByteArray &pcm16k, const QString &, Done done) override {
        audio = pcm16k;
        ++requests;
        done_ = std::move(done);
    }
    void cancel() override {
        if (done_) std::exchange(done_, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
    }
    bool pending() const { return bool(done_); }
    void answer(Result<QString> text) { std::exchange(done_, nullptr)(std::move(text)); }

    QByteArray audio;
    int requests{0};

private:
    bool local_;
    Done done_;
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
    tapped.microphone.play(testaudio::quiet(400) + testaudio::speech(600) + testaudio::quiet(1200));
    CHECK(tapped.pipeline.state() == VoiceState::Transcribing);      // the pause ended it
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
    rig.microphone.play(testaudio::quiet(500) + testaudio::speech(800) + testaudio::quiet(1200));
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

TEST_CASE("Wake word: only utterances that start with it become requests") {
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

    // The name alone: the next utterance is the command.
    rig.microphone.play(testaudio::speech(500) + testaudio::quiet(1200));
    local->answer(u"Hey Nexa."_s);
    CHECK(rig.pipeline.state() == VoiceState::Listening);
    rig.microphone.play(testaudio::speech(900) + testaudio::quiet(1200));
    local->answer(u"Open the calculator."_s);
    CHECK(rig.requests.last() == u"Open the calculator."_s);

    rig.pipeline.setSettings({.wakeWord = false});
    CHECK_FALSE(rig.microphone.active);
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
    rig.recognizer->answer(Error{ErrorCode::Network, u"OmniRoute is down"_s});
    CHECK(rig.pipeline.state() == VoiceState::Idle);
    REQUIRE(notices.size() == 1);
    CHECK(notices.at(0).at(0).toString().contains(u"OmniRoute is down"_s));
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
