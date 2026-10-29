// Pure voice helpers: WAV, format conversion, VAD, sentence chunking, wake word, transcripts,
// and the cloud request/response mapping.

#include "core/ai/ProviderRegistry.h"
#include "core/net/MultipartForm.h"
#include "voice/Wav.h"
#include "voice/capture/AudioConverter.h"
#include "voice/capture/EnergyVad.h"
#include "voice/capture/RecordedAudioInput.h"
#include "voice/pipeline/WakeWordDetector.h"
#include "voice/stt/CloudRecognizer.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"
#include "voice/tts/CloudSynthesizer.h"
#include "voice/tts/SentenceChunker.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QJsonArray>
#include <QSignalSpy>
#include <QTest>
#include <QJsonDocument>
#include <QJsonObject>
#include <QtEndian>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QStringList chunkAll(const QString &text, qsizetype step) {
    SentenceChunker chunker;
    QStringList out;
    for (qsizetype i = 0; i < text.size(); i += step) out += chunker.feed(QStringView(text).mid(i, step));
    out += chunker.finish();
    return out;
}

QByteArray floatWav(const QList<float> &samples, int rate) {
    QByteArray data(reinterpret_cast<const char *>(samples.constData()), samples.size() * 4);
    QByteArray out("RIFF");
    char word[4];
    qToLittleEndian<quint32>(quint32(36 + data.size()), word);
    out.append(word, 4).append("WAVEfmt ");
    qToLittleEndian<quint32>(16, word);
    out.append(word, 4);
    const quint16 fields[] = {3, 1};
    for (quint16 field : fields) {
        qToLittleEndian(field, word);
        out.append(word, 2);
    }
    for (quint32 field : {quint32(rate), quint32(rate * 4)}) {
        qToLittleEndian(field, word);
        out.append(word, 4);
    }
    for (quint16 field : {quint16(4), quint16(32)}) {
        qToLittleEndian(field, word);
        out.append(word, 2);
    }
    out.append("data");
    qToLittleEndian<quint32>(0xFFFFFFFFu, word);    // streamed: size unknown
    return out.append(word, 4).append(data);
}

} // namespace

TEST_CASE("wav round-trips 16-bit PCM") {
    const PcmAudio audio{.samples = testaudio::speech(250), .sampleRate = 16'000, .channels = 1};
    const QByteArray file = wav::encode(audio);
    CHECK(file.size() == 44 + audio.samples.size());
    CHECK(file.startsWith("RIFF"));
    const Result<PcmAudio> decoded = wav::decode(file);
    REQUIRE(decoded.isOk());
    CHECK(decoded.value().samples == audio.samples);
    CHECK(decoded.value().sampleRate == 16'000);
    CHECK(decoded.value().channels == 1);
    CHECK(decoded.value().durationMs() == 250);
}

TEST_CASE("wav decodes float samples with an unknown data size and skips other chunks") {
    QByteArray file = floatWav({0.0f, 0.5f, -1.0f, 2.0f}, 24'000);
    file.insert(12, QByteArray("LIST\x03\x00\x00\x00" "abc\x00", 12));   // odd chunk + pad byte
    const Result<PcmAudio> decoded = wav::decode(file);
    REQUIRE(decoded.isOk());
    REQUIRE(decoded.value().samples.size() == 8);
    const auto *samples = reinterpret_cast<const qint16 *>(decoded.value().samples.constData());
    CHECK(samples[0] == 0);
    CHECK(samples[1] == 16'384);
    CHECK(samples[2] == -32'767);
    CHECK(samples[3] == 32'767);    // clipped
    CHECK(decoded.value().sampleRate == 24'000);
}

TEST_CASE("wav rejects what it cannot play") {
    CHECK_FALSE(wav::decode("not a wav file at all").isOk());
    CHECK_FALSE(wav::decode(QByteArray("RIFF\x04\x00\x00\x00WAVE", 12)).isOk());
    PcmAudio audio{.samples = QByteArray(8, '\0')};
    QByteArray mp3InWav = wav::encode(audio);
    mp3InWav[20] = 0x55;    // format tag: MPEG Layer 3
    CHECK_FALSE(wav::decode(mp3InWav).isOk());
}

TEST_CASE("AudioConverter turns 48 kHz stereo float into 16 kHz mono Int16") {
    QAudioFormat format;
    format.setSampleRate(48'000);
    format.setChannelCount(2);
    format.setSampleFormat(QAudioFormat::Float);

    QList<float> interleaved;
    for (int i = 0; i < 4800; ++i) {        // 100 ms of a 1 kHz tone, same on both channels
        const float value = 0.5f * float(std::sin(2.0 * std::numbers::pi * 1000.0 * i / 48'000.0));
        interleaved << value << value;
    }
    const QByteArray input(reinterpret_cast<const char *>(interleaved.constData()), interleaved.size() * 4);

    AudioConverter whole(format);
    const QByteArray once = whole.convert(input);
    CHECK(once.size() == 1600 * 2);         // 100 ms at 16 kHz

    AudioConverter pieces(format);
    QByteArray chunked;
    for (qsizetype offset = 0; offset < input.size(); offset += 997) chunked += pieces.convert(input.mid(offset, 997));
    CHECK(chunked == once);

    // The tone survives: about 2 zero crossings per millisecond.
    const auto *samples = reinterpret_cast<const qint16 *>(once.constData());
    int crossings = 0;
    for (int i = 1; i < 1600; ++i) crossings += (samples[i - 1] < 0) != (samples[i] < 0);
    CHECK(crossings >= 190);
    CHECK(crossings <= 210);
}

TEST_CASE("AudioConverter passes 16 kHz mono Int16 through and upsamples 8 kHz") {
    QAudioFormat native;
    native.setSampleRate(16'000);
    native.setChannelCount(1);
    native.setSampleFormat(QAudioFormat::Int16);
    AudioConverter passthrough(native);
    CHECK(passthrough.isPassthrough());
    const QByteArray pcm = testaudio::speech(20);
    CHECK(passthrough.convert(pcm) == pcm);

    QAudioFormat narrow = native;
    narrow.setSampleRate(8'000);
    AudioConverter upsample(narrow);
    // 1600 input samples at 8 kHz → two outputs per sample, minus one sample of delay.
    CHECK(upsample.convert(testaudio::speech(100)).size() == (1600 - 1) * 2 * 2);
}

TEST_CASE("EnergyVad finds an utterance between quiet stretches") {
    EnergyVad vad;
    QList<EnergyVad::Event> events = vad.feed(testaudio::quiet(1000));
    CHECK(events.isEmpty());
    events = vad.feed(testaudio::speech(1000));
    REQUIRE(events == QList{EnergyVad::Event::SpeechStarted});
    CHECK(vad.inSpeech());
    events = vad.feed(testaudio::quiet(1500));
    REQUIRE(events == QList{EnergyVad::Event::SpeechEnded});
    const QByteArray utterance = vad.takeUtterance();
    // Pre-roll (300 ms) + speech (1000 ms) + kept trailing quiet (200 ms).
    const qint64 ms = utterance.size() / 32;
    CHECK(ms >= 1450);
    CHECK(ms <= 1560);
    CHECK(vad.heardSpeech());
    CHECK(vad.takeUtterance().isEmpty());
}

TEST_CASE("EnergyVad reports where the utterance was") {
    EnergyVad vad;
    QList<EnergyVad::Event> events = vad.feed(testaudio::quiet(500) + testaudio::speech(600));
    REQUIRE(events == QList{EnergyVad::Event::SpeechStarted});
    // The speech starts at 500 ms; the utterance keeps 300 ms before it.
    CHECK(std::abs(vad.utteranceStart() - 3200) <= 320);
    events = vad.feed(testaudio::quiet(1200));
    REQUIRE(events == QList{EnergyVad::Event::SpeechEnded});
    const EnergyVad::Span span = vad.lastSpan();
    CHECK(std::abs(span.start - 3200) <= 320);
    // The speech ends at 1100 ms; 200 ms of quiet are kept after it.
    CHECK(std::abs(span.end - 20800) <= 320);
    CHECK(span.end - span.start == vad.takeUtterance().size() / 2);
}

TEST_CASE("EnergyVad ignores clicks and gives the same result for any chunking") {
    EnergyVad vad;
    QByteArray audio = testaudio::quiet(800) + testaudio::speech(60) + testaudio::quiet(800);
    CHECK(vad.feed(audio).isEmpty());
    CHECK_FALSE(vad.heardSpeech());

    const QByteArray talk = testaudio::quiet(700) + testaudio::speech(500) + testaudio::quiet(300)
                          + testaudio::speech(400) + testaudio::quiet(1500);
    EnergyVad once;
    const QList<EnergyVad::Event> expected = once.feed(talk);
    CHECK(expected == QList{EnergyVad::Event::SpeechStarted, EnergyVad::Event::SpeechEnded});   // one utterance
    EnergyVad chunked;
    QList<EnergyVad::Event> events;
    for (qsizetype offset = 0; offset < talk.size(); offset += 333) events += chunked.feed(talk.mid(offset, 333));
    CHECK(events == expected);
    CHECK(chunked.takeUtterance() == once.takeUtterance());
}

TEST_CASE("EnergyVad cuts endless speech and adapts to a noisy room") {
    EnergyVad::Settings settings;
    settings.maxUtteranceMs = 2'000;
    EnergyVad vad(settings);
    const QList<EnergyVad::Event> events = vad.feed(testaudio::quiet(500) + testaudio::speech(3000));
    CHECK(events.count(EnergyVad::Event::SpeechEnded) == 1);

    EnergyVad room;
    CHECK(room.feed(testaudio::make(3000, 0.0, 0.05)).isEmpty());        // steady fan noise
    CHECK(room.feed(testaudio::make(800, 0.5, 0.05)).contains(EnergyVad::Event::SpeechStarted));
}

TEST_CASE("EnergyVad::containsSpeech judges whole recordings") {
    CHECK(EnergyVad::containsSpeech(testaudio::speech(600) + testaudio::quiet(400)));     // spoken at once
    CHECK(EnergyVad::containsSpeech(testaudio::quiet(300) + testaudio::speech(400) + testaudio::quiet(300)));
    CHECK(EnergyVad::containsSpeech(testaudio::speech(600)));                             // talking throughout
    CHECK(EnergyVad::containsSpeech(testaudio::quiet(300) + testaudio::make(400, 0.006, 0.002)));  // soft voice
    CHECK_FALSE(EnergyVad::containsSpeech(testaudio::quiet(2000)));
    CHECK_FALSE(EnergyVad::containsSpeech(testaudio::make(2000, 0.0, 0.01)));            // noise only
    CHECK_FALSE(EnergyVad::containsSpeech(testaudio::speech(60)));
}

TEST_CASE("EnergyVad::trimSilence keeps the speech and short margins") {
    const QByteArray recording = testaudio::quiet(1000) + testaudio::speech(500) + testaudio::quiet(1000);
    const QByteArray trimmed = EnergyVad::trimSilence(recording, EnergyVad::Settings{});
    // 300 ms before, the speech, and 200 + 100 ms after.
    CHECK(trimmed.size() / 32 >= 1050);
    CHECK(trimmed.size() / 32 <= 1150);
    CHECK(EnergyVad::containsSpeech(trimmed));
    const QByteArray silence = testaudio::quiet(1000);
    CHECK(EnergyVad::trimSilence(silence, EnergyVad::Settings{}) == silence);
}

TEST_CASE("SentenceChunker emits sentences as soon as they are complete") {
    SentenceChunker chunker;
    CHECK(chunker.feed(u"Hello there. How") == QStringList{u"Hello there."_s});
    CHECK(chunker.feed(u" are you").isEmpty());
    CHECK(chunker.feed(u"? Fine") == QStringList{u"How are you?"_s});
    CHECK(chunker.finish() == QStringList{u"Fine"_s});

    SentenceChunker first;
    CHECK(first.feed(u"Sure! Opening it now.") == QStringList{u"Sure!"_s});
    CHECK(first.feed(u" Done") == QStringList{u"Opening it now."_s});
}

TEST_CASE("SentenceChunker gives the same sentences for any chunking") {
    const QString reply = u"Sure! I opened **Firefox** for you. It costs $3.50, e.g. less than before.\n"
                          "Steps:\n1. Open `settings`. 2. Click Wi-Fi.\n- Then [read this](https://example.com/a?b=c).\n"
                          "```bash\nls -la\n```\nThat's all, Mr. Smith! Bye"_s;
    const QStringList expected{
        u"Sure!"_s,
        u"I opened Firefox for you."_s,
        u"It costs $3.50, e.g. less than before."_s,
        u"Steps:"_s,
        u"Open settings."_s,
        u"2. Click Wi-Fi."_s,
        u"Then read this."_s,
        u"That's all, Mr. Smith!"_s,
        u"Bye"_s,
    };
    CHECK(chunkAll(reply, reply.size()) == expected);
    CHECK(chunkAll(reply, 1) == expected);
    CHECK(chunkAll(reply, 7) == expected);
}

TEST_CASE("SentenceChunker cleans Markdown for speech") {
    CHECK(SentenceChunker::speakable(u"## Install *the* tools") == u"Install the tools"_s);
    CHECK(SentenceChunker::speakable(u"> - [x] done with __this__") == u"done with this"_s);
    CHECK(SentenceChunker::speakable(u"See <https://www.qt.io/download> or https://kde.org/x") == u"See qt.io or kde.org"_s);
    CHECK(SentenceChunker::speakable(u"Use my_var_name, not ~~that~~") == u"Use my_var_name, not that"_s);
    CHECK(SentenceChunker::speakable(u"| Name | Size |") == u"Name, Size"_s);
    CHECK(SentenceChunker::speakable(u"|---|:---:|").isEmpty());
    CHECK(SentenceChunker::speakable(u"---").isEmpty());
    CHECK(SentenceChunker::speakable(u"Line<br>break") == u"Line break"_s);
    CHECK(SentenceChunker::speakable(u"- not a list", true) == u"- not a list"_s);
}

TEST_CASE("SentenceChunker skips code blocks and splits long lines") {
    CHECK(chunkAll(u"Run:\n```\nrm -rf build. Then.\n```\nOK."_s, 3) == QStringList{u"Run:"_s, u"OK."_s});
    const QString longLine = QString(u"word, "_s).repeated(80);
    const QStringList parts = chunkAll(longLine, 5);
    CHECK(parts.size() >= 2);
    for (const QString &part : parts) CHECK(part.size() <= 330);
}

TEST_CASE("WakeWordDetector finds any name anywhere") {
    const WakeWordDetector nexa(u"Nexa"_s);
    const auto mid = nexa.find(u"I was just saying that, hey Nexa, pause the music."_s);
    REQUIRE(mid);
    CHECK(mid->command == u"pause the music."_s);
    CHECK(mid->firstWord == 5);
    CHECK(mid->greeted);
    CHECK(nexa.find(u"Hey Nexa, open Firefox."_s)->command == u"open Firefox."_s);
    CHECK(nexa.find(u"Hey Nexus, what time is it?"_s)->command == u"what time is it?"_s);
    CHECK(nexa.find(u"heynecks, pause it"_s)->command == u"pause it"_s);
    CHECK(nexa.find(u"OK Neksa. Play some music."_s)->command == u"Play some music."_s);
    CHECK(nexa.find(u"That's it. Nexa, stop."_s)->command == u"stop."_s);
    CHECK(nexa.find(u"Nexa open files"_s)->command == u"open files"_s);
    CHECK(nexa.find(u"  Hey,  Nexa!"_s)->command.isEmpty());
    CHECK_FALSE(nexa.find(u"Nexa open files"_s)->greeted);
    for (const QString &text : {u"Next song please"_s, u"The Nexus phone is really good"_s, u"I need an extra hour"_s,
                                u"I told Nexa nothing"_s, u"Hey Nick, can you pass the salt?"_s,
                                u"Hey, can you hear me?"_s}) {
        INFO(text.toStdString());
        CHECK_FALSE(nexa.find(text));
    }

    const WakeWordDetector jarvis(u"Jarvis"_s);
    CHECK(jarvis.find(u"Hey Jervis, open YouTube"_s)->command == u"open YouTube"_s);
    CHECK_FALSE(jarvis.find(u"I love Jarvis movies"_s));
    CHECK(jarvis.commandOf(u"Hey Jarvis."_s).isEmpty());
    CHECK(jarvis.commandOf(u"Hey Jarvis, open YouTube"_s) == u"open YouTube"_s);
    CHECK(jarvis.commandOf(u"open YouTube"_s) == u"open YouTube"_s);
    CHECK(WakeWordDetector(u"Mister Robot"_s).find(u"hey mister robot, lights off"_s)->command == u"lights off"_s);

    CHECK(WakeWordDetector::soundKey(u"Nexa"_s) == u"nks"_s);
    CHECK(WakeWordDetector::soundKey(u"Necksa"_s) == u"nks"_s);
    CHECK(WakeWordDetector::soundKey(u"Jarvis"_s) == u"jrvs"_s);
    CHECK(nexa.recognizerPrompt() == u"Hey Nexa."_s);
    CHECK(jarvis.name() == u"Jarvis"_s);

    // Saying the name at all, as Nexa may in its own replies.
    CHECK(nexa.mentions(u"I am Nexa, your assistant."_s));
    CHECK(nexa.mentions(u"I'm Necksa."_s));
    CHECK_FALSE(nexa.mentions(u"The weather is fine today."_s));
}

namespace {

// A recogniser that knows no word times: transcribeWords() must still answer through transcribe().
class PlainRecognizer : public ISpeechRecognizer {
public:
    explicit PlainRecognizer(Result<QString> answer) : answer_(std::move(answer)) {}
    QString name() const override { return u"plain"_s; }
    bool isLocal() const override { return true; }
    void transcribe(const QByteArray &, const QString &, Done done) override { done(answer_); }
    void cancel() override {}

private:
    Result<QString> answer_;
};

} // namespace

TEST_CASE("A recogniser without word times still answers transcribeWords") {
    PlainRecognizer plain(QString(u"hello there"_s));
    std::optional<Result<TimedTranscript>> got;
    plain.transcribeWords(QByteArray(3200, 0), u"Hey Nexa."_s, [&got](Result<TimedTranscript> result) { got = result; });
    REQUIRE(got);
    REQUIRE(got->isOk());
    CHECK(got->value().text == u"hello there"_s);
    CHECK(got->value().words.isEmpty());

    PlainRecognizer failing(Error{ErrorCode::Network, u"offline"_s});
    std::optional<Result<TimedTranscript>> failed;
    failing.transcribeWords(QByteArray(3200, 0), QString(), [&failed](Result<TimedTranscript> result) { failed = result; });
    REQUIRE(failed);
    REQUIRE_FALSE(failed->isOk());
    CHECK(failed->error().code == ErrorCode::Network);
}

TEST_CASE("RecordedAudioInput plays in real time, then silence") {
    RecordedAudioInput recording;
    QList<QByteArray> chunks;
    QObject::connect(&recording, &IAudioInput::audio, &recording, [&chunks](const QByteArray &pcm) { chunks.append(pcm); });
    QSignalSpy drained(&recording, &RecordedAudioInput::drained);
    REQUIRE(recording.start().isOk());
    recording.play(testaudio::speech(1000));
    CHECK(recording.playing());
    QTest::qWait(1250);
    REQUIRE(chunks.size() >= 11);
    for (int i = 0; i < 10; ++i) CHECK(chunks[i] == testaudio::speech(1000).mid(i * 3200, 3200));
    CHECK(chunks[10] == QByteArray(3200, '\0'));         // the recording is over: silence
    REQUIRE(drained.wait(6000));
    CHECK_FALSE(recording.playing());
    recording.stop();
    CHECK_FALSE(recording.isActive());
}

TEST_CASE("cleanTranscript drops non-speech annotations") {
    CHECK(cleanTranscript(u" [BLANK_AUDIO] "_s).isEmpty());
    CHECK(cleanTranscript(u"(music) ♪♪"_s).isEmpty());
    CHECK(cleanTranscript(u" ..."_s).isEmpty());
    CHECK(cleanTranscript(u"  Open   the [noise] calculator. "_s) == u"Open the calculator."_s);
    CHECK(cleanTranscript(u"*laughs* Pause it"_s) == u"Pause it"_s);
}

TEST_CASE("MultipartForm builds a form-data body") {
    MultipartForm form("XyZ");
    form.addField(u"model"_s, u"whisper-1"_s);
    form.addFile(u"file"_s, u"speech \"1\".wav"_s, "audio/wav", "RIFF");
    CHECK(form.contentType() == "multipart/form-data; boundary=XyZ");
    CHECK(form.body() == "--XyZ\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\nwhisper-1\r\n"
                         "--XyZ\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech \\\"1\\\".wav\"\r\n"
                         "Content-Type: audio/wav\r\n\r\nRIFF\r\n--XyZ--\r\n");
    CHECK(MultipartForm().contentType() != MultipartForm().contentType());     // random boundaries
}

TEST_CASE("CloudRecognizer picks the endpoint by model and parses both replies") {
    CHECK(CloudRecognizer::usesTranscriptionEndpoint(u"whisper-1"_s));
    CHECK(CloudRecognizer::usesTranscriptionEndpoint(u"openrouter/openai/gpt-4o-mini-transcribe"_s));
    CHECK(CloudRecognizer::usesTranscriptionEndpoint(u"deepgram/nova-3"_s));
    CHECK(CloudRecognizer::usesTranscriptionEndpoint(u"qwen/qwen3-asr"_s));
    CHECK_FALSE(CloudRecognizer::usesTranscriptionEndpoint(u"anyapi/google/gemini-2.5-flash-lite"_s));
    CHECK_FALSE(CloudRecognizer::usesTranscriptionEndpoint(u"gpt-audio-mini"_s));

    const CloudRecognizer::Settings settings{.model = u"gemini"_s, .language = u"en"_s};
    const QJsonObject body = QJsonDocument::fromJson(CloudRecognizer::chatBody(settings, "RIFF", u"Hey Nexa."_s)).object();
    CHECK(body.value(u"model"_s).toString() == u"gemini"_s);
    CHECK_FALSE(body.value(u"stream"_s).toBool(true));
    const QJsonArray messages = body.value(u"messages"_s).toArray();
    REQUIRE(messages.size() == 2);
    CHECK(messages[0].toObject().value(u"content"_s).toString().contains(u"Hey Nexa."_s));
    const QJsonObject audio = messages[1].toObject().value(u"content"_s).toArray()[1].toObject();
    CHECK(audio.value(u"type"_s).toString() == u"input_audio"_s);
    CHECK(audio.value(u"input_audio"_s).toObject().value(u"data"_s).toString() == u"UklGRg=="_s);
    CHECK(audio.value(u"input_audio"_s).toObject().value(u"format"_s).toString() == u"wav"_s);

    const QByteArray form = CloudRecognizer::transcriptionForm(settings, "RIFF", {}).body();
    CHECK(form.contains("name=\"language\"\r\n\r\nen\r\n"));
    CHECK(form.contains("filename=\"speech.wav\""));

    CHECK(CloudRecognizer::parseTranscription(R"({"text":" Open Firefox. "})").value() == u"Open Firefox."_s);
    CHECK(CloudRecognizer::parseTranscription("plain transcript\n").value() == u"plain transcript"_s);
    CHECK_FALSE(CloudRecognizer::parseTranscription(R"({"error":"x"})").isOk());
    CHECK(CloudRecognizer::parseChatReply(R"({"choices":[{"message":{"content":"\"Open the calculator.\""}}]})").value()
          == u"Open the calculator."_s);
    CHECK(CloudRecognizer::parseChatReply(R"({"choices":[{"message":{"content":"Transcript: pause"}}]})").value()
          == u"pause"_s);
    CHECK(CloudRecognizer::parseChatReply(R"({"choices":[{"message":{"content":[{"type":"text","text":"hi"}]}}]})")
              .value() == u"hi"_s);
    const Result<QString> refused = CloudRecognizer::parseChatReply(R"({"error":{"message":"no audio support"}})");
    REQUIRE_FALSE(refused.isOk());
    CHECK(refused.error().message == u"no audio support"_s);
}

TEST_CASE("CloudSynthesizer asks for WAV with the configured voice and speed") {
    CloudSynthesizer::Settings settings{.model = u"gtts/default"_s, .voice = u"en"_s};
    QJsonObject body = QJsonDocument::fromJson(CloudSynthesizer::requestBody(settings, u"Hi there."_s)).object();
    CHECK(body.value(u"model"_s).toString() == u"gtts/default"_s);
    CHECK(body.value(u"input"_s).toString() == u"Hi there."_s);
    CHECK(body.value(u"voice"_s).toString() == u"en"_s);
    CHECK(body.value(u"response_format"_s).toString() == u"wav"_s);
    CHECK_FALSE(body.contains(u"speed"_s));
    settings.rate = 1.0;
    body = QJsonDocument::fromJson(CloudSynthesizer::requestBody(settings, u"Hi."_s)).object();
    CHECK(body.value(u"speed"_s).toDouble() == 2.0);
}

TEST_CASE("ProviderRegistry::upgrade gives v1 providers the preset speech models") {
    Config config;
    config.version = 1;
    ProviderPreset openAiPreset;
    for (const ProviderPreset &preset : ProviderRegistry::presets()) {
        if (preset.id == u"openai"_s) openAiPreset = preset;
    }
    REQUIRE(openAiPreset.id == u"openai"_s);
    ProviderConfig openai = ProviderRegistry::fromPreset(openAiPreset);
    openai.sttModel.clear();
    openai.ttsModel.clear();
    openai.ttsVoice.clear();
    openai.apiKey = u"sk-kept"_s;
    config.providers = {openai, ProviderConfig{.id = u"mine"_s, .baseUrl = u"http://localhost:1/v1"_s}};
    const auto nothingInstalled = [](const QString &) { return false; };

    ProviderRegistry::upgrade(config, nothingInstalled);
    CHECK(config.version == Config::kVersion);
    CHECK(config.providers[0].sttModel == u"whisper-1"_s);
    CHECK(config.providers[0].ttsModel == u"tts-1"_s);
    CHECK(config.providers[0].ttsVoice == u"alloy"_s);
    CHECK(config.providers[0].apiKey == u"sk-kept"_s);
    CHECK(config.providers[1].sttModel.isEmpty());

    config.providers[0].ttsModel.clear();     // a v2 choice to have no cloud voice stays
    ProviderRegistry::upgrade(config, nothingInstalled);
    CHECK(config.providers[0].ttsModel.isEmpty());
}
