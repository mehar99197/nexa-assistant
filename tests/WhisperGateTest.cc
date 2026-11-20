// The installed whisper model against silence and speech: silence and a quiet room give no words (whisper's own
// no-speech and confidence figures, and what it is known to make up), speech still gives its words. Skipped without the model; the
// speech clip is one of M16's Piper recordings, skipped when this checkout has none.

#include "voice/VoiceBackends.h"
#include "voice/Wav.h"
#include "voice/capture/AudioConverter.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/stt/Transcript.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QAudioFormat>
#include <QFile>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

std::optional<TimedTranscript> hear(ISpeechRecognizer &recognizer, const QByteArray &pcm) {
    std::optional<Result<TimedTranscript>> result;
    recognizer.transcribeWords(pcm, u"Hey Nexa."_s, [&result](Result<TimedTranscript> r) { result = std::move(r); });
    if (!QTest::qWaitFor([&result] { return result.has_value(); }, 60'000) || !result->isOk()) return std::nullopt;
    return result->value();
}

QByteArray clip(const QString &name) {
    QFile file(QStringLiteral(NEXA_TESTS_DIR "/../.superpowers/scratch/kws/clips/") + name);
    if (!file.open(QIODevice::ReadOnly)) return {};
    const Result<PcmAudio> audio = wav::decode(file.readAll());
    if (!audio) return {};
    QAudioFormat format;
    format.setSampleRate(audio.value().sampleRate);
    format.setChannelCount(audio.value().channels);
    format.setSampleFormat(QAudioFormat::Int16);
    AudioConverter converter(format);
    return converter.convert(audio.value().samples);
}

} // namespace

TEST_CASE("Whisper gives no words for silence, and still hears speech", "[whisper]") {
    VoiceConfig voice;
    Result<std::unique_ptr<ISpeechRecognizer>> made = VoiceBackends::createLocalRecognizer(voice);
    if (!made) SKIP("no local recognition here: " << made.error().message.toStdString());
    ISpeechRecognizer &recognizer = *made.value();

    // Silence and a quiet room give no words. (White noise does: base.en invents "Okay." or "Let's go." from it, with
    // a confidence of 0.6–0.8 and no-speech near 0 — so whisper is never the noise filter; the voice detector and the
    // owner's voice come before it.)
    for (const QByteArray &nothing : {QByteArray(3 * 16'000 * 2, '\0'), testaudio::quiet(3000, 5)}) {
        const std::optional<TimedTranscript> heard = hear(recognizer, nothing);
        REQUIRE(heard.has_value());
        INFO("whisper wrote: " << heard->text.toStdString());
        CHECK(cleanTranscript(heard->text).isEmpty());
    }
    const QByteArray speech = clip(u"pos-nexa-start-lessac-1.0-clean.wav"_s);
    if (speech.isEmpty()) SKIP("no recorded speech in this checkout (.superpowers/scratch/kws/clips)");
    const std::optional<TimedTranscript> said = hear(recognizer, speech);
    REQUIRE(said.has_value());
    CHECK_FALSE(cleanTranscript(said->text).isEmpty());
    CHECK(said->confidence > 0.4);
}
