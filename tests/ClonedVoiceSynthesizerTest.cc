// voice/tts/ClonedVoiceSynthesizer: Nexa speaking in a voice from a sample — refused plainly when ZipVoice is not
// installed, and, where it is (scripts/download-sherpa.sh, with Piper for the sample and the sounds), run for real:
// a sentence comes out in the sample's voice, of a plausible length, and whisper hears its words.

#include "voice/VoiceBackends.h"
#include "voice/capture/AudioConverter.h"
#include "voice/stt/ISpeechRecognizer.h"
#include "voice/tts/ClonedVoiceSynthesizer.h"
#include "voice/tts/PiperSynthesizer.h"
#include "voice/tts/VoiceSample.h"
#include "voice/wake/SherpaRuntime.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QAudioFormat>
#include <QFileInfo>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstdlib>
#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kModelWaitMs = 180'000;

template <typename T>
std::optional<Result<T>> waitFor(const std::function<void(std::function<void(Result<T>)>)> &start, int ms) {
    std::optional<Result<T>> answer;
    start([&answer](Result<T> result) { answer = std::move(result); });
    (void)QTest::qWaitFor([&answer] { return answer.has_value(); }, ms);
    return answer;
}

} // namespace

TEST_CASE("Without ZipVoice the voice from a sample fails with the install step, and cancel answers at once") {
    SherpaRuntime runtime(u"/nonexistent/nexa-sherpa"_s);
    VoiceSample sample;
    sample.audio = PcmAudio{testaudio::speech(4'000), testaudio::kRate, 1};
    sample.text = VoiceSample::passage();
    ClonedVoiceSynthesizer voice(&runtime, sample, Phonemizer(u"/nonexistent/nexa-piper"_s), 0.0);
    CHECK(voice.name().contains(u"my voice"_s));
    const std::optional<Result<PcmAudio>> clip = waitFor<PcmAudio>(
        [&voice](std::function<void(Result<PcmAudio>)> done) { voice.synthesize(u"Hello."_s, std::move(done)); }, 5'000);
    REQUIRE(clip.has_value());
    REQUIRE_FALSE(*clip);
    CHECK(clip->error().message.contains(u"scripts/download-sherpa.sh"_s));

    std::optional<Result<PcmAudio>> cancelled;
    voice.synthesize(u"Hello again."_s, [&cancelled](Result<PcmAudio> result) { cancelled = std::move(result); });
    voice.cancel();
    REQUIRE(cancelled.has_value());
    CHECK(cancelled->error().code == ErrorCode::Cancelled);
    QTest::qWait(50);       // the worker's late answer for the cancelled sentence is dropped, not delivered twice
}

TEST_CASE("Installed ZipVoice: a sentence in the sample's voice, and whisper hears its words", "[sherpa][slow]") {
    SherpaRuntime runtime;
    if (!runtime.load() || !runtime.canCloneVoice()) SKIP("ZipVoice is not installed: " << runtime.problem().toStdString());
    const Phonemizer phonemizer(VoiceBackends::piperDir());
    if (!phonemizer.available()) SKIP("Piper is not installed: " << phonemizer.problem().toStdString());
    const QStringList voices = VoiceBackends::piperVoices();
    if (voices.isEmpty()) SKIP("Piper has no voice installed to make a sample with");

    // The sample: Piper reading the passage stands in for the owner.
    PiperSynthesizer piper(VoiceBackends::piperBinary(), VoiceBackends::piperVoicePath(voices.first()), 0.0);
    const std::optional<Result<PcmAudio>> read = waitFor<PcmAudio>(
        [&piper](std::function<void(Result<PcmAudio>)> done) { piper.synthesize(VoiceSample::passage(), std::move(done)); },
        60'000);
    REQUIRE(read.has_value());
    REQUIRE(*read);
    VoiceSample sample;
    sample.audio = VoiceSample::trimmed(read->value());
    sample.text = VoiceSample::passage();
    REQUIRE(VoiceSample::usable(sample.audio).isOk());

    ClonedVoiceSynthesizer voice(&runtime, sample, phonemizer, 0.0);
    const QString sentence = u"Hello from Nexa. Today the weather is bright and clear."_s;
    const std::optional<Result<PcmAudio>> clip = waitFor<PcmAudio>(
        [&voice, &sentence](std::function<void(Result<PcmAudio>)> done) { voice.synthesize(sentence, std::move(done)); },
        kModelWaitMs);
    REQUIRE(clip.has_value());
    REQUIRE(*clip);
    const PcmAudio &spoken = clip->value();
    CHECK(spoken.channels == 1);
    CHECK(spoken.sampleRate == 24'000);
    // About nine words: between two and eight seconds of speech, and not silence.
    CHECK(spoken.durationMs() >= 2'000);
    CHECK(spoken.durationMs() <= 8'000);
    const auto *samples = reinterpret_cast<const qint16 *>(spoken.samples.constData());
    qint16 peak = 0;
    for (qsizetype i = 0; i < spoken.frameCount(); ++i) peak = std::max<qint16>(peak, qint16(std::abs(int(samples[i]))));
    CHECK(peak > 2'000);

    // What whisper makes of it, when it is here: the words came through.
    if (!VoiceBackends::whisperBuilt() || !QFileInfo::exists(VoiceBackends::whisperModelPath(VoiceConfig{}.whisperModel))) {
        WARN("whisper is not here: the words of the spoken sentence were not checked");
        return;
    }
    Result<std::unique_ptr<ISpeechRecognizer>> whisper = VoiceBackends::createLocalRecognizer(VoiceConfig{});
    REQUIRE(whisper);
    QAudioFormat format;
    format.setSampleRate(spoken.sampleRate);
    format.setChannelCount(1);
    format.setSampleFormat(QAudioFormat::Int16);
    const QByteArray pcm16k = AudioConverter(format).convert(spoken.samples);
    const std::optional<Result<QString>> heard = waitFor<QString>(
        [&whisper, &pcm16k](std::function<void(Result<QString>)> done) {
            whisper.value()->transcribe(pcm16k, QString(), std::move(done));
        },
        kModelWaitMs);
    REQUIRE(heard.has_value());
    REQUIRE(*heard);
    const QString words = heard->value().toLower();
    WARN("whisper heard the cloned voice say: " << words.toStdString());
    int matched = 0;
    for (const QString &word : {u"weather"_s, u"bright"_s, u"clear"_s, u"today"_s, u"hello"_s}) {
        if (words.contains(word)) ++matched;
    }
    CHECK(matched >= 2);
}
