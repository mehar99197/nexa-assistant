// voice/listen/SpeakerGate and VoiceProfile: the profile's arithmetic and file (always), and the installed speaker model
// telling three voices apart — clean, over a fan at 5 dB and under a TV — which is what the strictness thresholds are
// measured against ([speaker]: skipped without the model or without M16's recordings in this checkout).

#include "voice/Wav.h"
#include "voice/capture/AudioConverter.h"
#include "voice/listen/SpeakerGate.h"
#include "voice/listen/VoiceProfile.h"
#include "voice/wake/SherpaRuntime.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QAudioFormat>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <iostream>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QByteArray clip(const QString &relative) {
    QFile file(QStringLiteral(NEXA_TESTS_DIR "/../.superpowers/scratch/kws/") + relative);
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

std::optional<QList<float>> embed(SpeakerGate &gate, const QByteArray &pcm) {
    std::optional<std::optional<QList<float>>> result;
    gate.embed(pcm, [&result](std::optional<QList<float>> e) { result = std::move(e); });
    if (!QTest::qWaitFor([&result] { return result.has_value(); }, 30'000)) return std::nullopt;
    return *result;
}

} // namespace

TEST_CASE("A voice profile is the mean direction of its recordings, kept privately") {
    const VoiceProfile profile = VoiceProfile::fromRecordings(u"model.onnx"_s, {{1, 0, 0}, {0.8f, 0.2f, 0}, {}});
    CHECK(profile.samples == 2);
    CHECK(VoiceProfile::cosine(profile.embedding, {1, 0.1f, 0}) > 0.99);
    CHECK(VoiceProfile::cosine({1, 0}, {0, 1}) == 0.0);
    CHECK(VoiceProfile::cosine({1, 0}, {1, 0, 0}) == -1.0);

    QTemporaryDir dir;
    const QString path = dir.filePath(u"nexa/voice-profile.json"_s);
    REQUIRE(profile.save(path).isOk());
    CHECK_FALSE(QFileInfo(path).permissions().testAnyFlags(QFileDevice::ReadGroup | QFileDevice::ReadOther));
    const Result<VoiceProfile> loaded = VoiceProfile::load(path);
    REQUIRE(loaded.isOk());
    CHECK(loaded.value().model == u"model.onnx"_s);
    CHECK(loaded.value().embedding == profile.embedding);
    REQUIRE(VoiceProfile::forget(path).isOk());
    CHECK(VoiceProfile::load(path).error().code == ErrorCode::NotFound);
}

TEST_CASE("Without a profile the gate lets every voice through at once") {
    SherpaRuntime runtime(u"/nonexistent"_s);
    SpeakerGate gate(&runtime);
    CHECK_FALSE(gate.active());
    std::optional<bool> owner;
    gate.check(testaudio::speech(2000), [&owner](bool o, double) { owner = o; });
    REQUIRE(owner.has_value());
    CHECK(*owner);
    CHECK(SpeakerGate::thresholdFor(u"low"_s) < SpeakerGate::thresholdFor(u"normal"_s));
    CHECK(SpeakerGate::thresholdFor(u"normal"_s) < SpeakerGate::thresholdFor(u"high"_s));
}

TEST_CASE("The speaker model tells three voices apart, clean, over a fan and under a TV", "[speaker]") {
    SherpaRuntime runtime;
    if (!runtime.load() || !runtime.canTellSpeakers()) SKIP("the speaker model is not installed (scripts/download-sherpa.sh)");
    if (clip(u"clips/pos-nexa-start-lessac-1.0-clean.wav"_s).isEmpty()) SKIP("no recordings in this checkout");
    SpeakerGate gate(&runtime);
    const QStringList voices{u"lessac"_s, u"ryan"_s, u"alan"_s};
    const auto probes = [](const QString &voice) {
        return QStringList{u"clips/neg-alexa-%1-1.0.wav"_s.arg(voice),          u"clips/neg-exam-%1-1.0.wav"_s.arg(voice),
                           u"clips/neg-next-%1-1.0.wav"_s.arg(voice),           u"clips/pos-nexa-mid-%1-1.0-clean.wav"_s.arg(voice),
                           u"clips/pos-nexa-mid-%1-1.0-noise.wav"_s.arg(voice), u"clips/pos-nexa-mid-%1-1.0-tv.wav"_s.arg(voice),
                           u"fan/neg-alexa-%1-1.0-snr5.wav"_s.arg(voice),       u"fan/neg-exam-%1-1.0-snr5.wav"_s.arg(voice)};
    };
    double lowestSame = 1.0;
    double highestOther = -1.0;
    for (const QString &owner : voices) {
        QList<QList<float>> recordings;
        for (const QString &speed : {u"0.85"_s, u"1.0"_s, u"1.25"_s}) {
            const std::optional<QList<float>> e = embed(gate, clip(u"clips/pos-nexa-start-%1-%2-clean.wav"_s.arg(owner, speed)));
            REQUIRE(e.has_value());
            recordings.append(*e);
        }
        gate.setProfile(VoiceProfile::fromRecordings(gate.modelName(), recordings));
        const auto scoreOf = [&gate](const QByteArray &pcm) {
            std::optional<std::optional<double>> result;
            gate.score(pcm, [&result](std::optional<double> s) { result = s; });
            REQUIRE(QTest::qWaitFor([&result] { return result.has_value(); }, 60'000));
            REQUIRE(result->has_value());
            return **result;
        };
        for (const QString &speaker : voices) {
            for (const QString &probe : probes(speaker)) {
                const QByteArray pcm = clip(probe);
                if (pcm.isEmpty()) continue;
                const double score = scoreOf(pcm);
                std::cout << owner.toStdString() << " vs " << probe.toStdString() << ": " << score << '\n';
                // The "TV" under those recordings reads in Alan's voice: Alan is in them too.
                const bool ownerSpeaks = speaker == owner || (owner == u"alan"_s && probe.endsWith(u"-tv.wav"_s));
                if (ownerSpeaks) {
                    lowestSame = std::min(lowestSame, score);
                } else {
                    highestOther = std::max(highestOther, score);
                }
            }
        }
        // Noise alone is nobody.
        highestOther = std::max(highestOther, scoreOf(testaudio::make(3000, 0.0, 0.1, 9)));
    }
    std::cout << "lowest same-voice score " << lowestSame << ", highest other " << highestOther << '\n';
    CHECK(lowestSame > SpeakerGate::thresholdFor(u"high"_s));
    CHECK(highestOther < SpeakerGate::thresholdFor(u"low"_s));
}

// Hidden (run by name): NEXA_PROFILE_OUT=<file> writes a profile of one of M16's recorded voices (NEXA_PROFILE_VOICE,
// "lessac" by default), to try the whole path with --hear on a private copy without anyone's real voice.
TEST_CASE("Write a voice profile from recordings", "[.enrol-from-clips]") {
    const QString out = qEnvironmentVariable("NEXA_PROFILE_OUT");
    if (out.isEmpty()) SKIP("NEXA_PROFILE_OUT is not set");
    const QString voice = qEnvironmentVariable("NEXA_PROFILE_VOICE", u"lessac"_s);
    SherpaRuntime runtime;
    REQUIRE(runtime.load());
    REQUIRE(runtime.canTellSpeakers());
    SpeakerGate gate(&runtime);
    QList<QList<float>> recordings;
    for (const QString &speed : {u"0.85"_s, u"1.0"_s, u"1.25"_s}) {
        const std::optional<QList<float>> e = embed(gate, clip(u"clips/pos-nexa-start-%1-%2-clean.wav"_s.arg(voice, speed)));
        REQUIRE(e.has_value());
        recordings.append(*e);
    }
    REQUIRE(VoiceProfile::fromRecordings(gate.modelName(), recordings).save(out).isOk());
}
