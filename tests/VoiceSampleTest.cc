// voice/tts/VoiceSample: the recording of the owner's voice Nexa speaks from — trimmed of its quiet, judged usable or
// not, kept 0600 beside its words, and forgotten.

#include "voice/tts/VoiceSample.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QFile>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

PcmAudio audio(const QByteArray &pcm) {
    return PcmAudio{pcm, testaudio::kRate, 1};
}

} // namespace

TEST_CASE("The passage the owner reads is a few sentences of English, Nexa's name among them") {
    const QString passage = VoiceSample::passage();
    CHECK(passage.split(u' ', Qt::SkipEmptyParts).size() >= 20);
    CHECK(passage.contains(u"Nexa"_s));
    CHECK(passage.endsWith(u'.'));
}

TEST_CASE("Trimming keeps the speech and a breath around it, never more than the model takes") {
    const PcmAudio recording = audio(testaudio::quiet(800) + testaudio::speech(2'400) + testaudio::quiet(1'200));
    const PcmAudio trimmed = VoiceSample::trimmed(recording);
    CHECK(trimmed.sampleRate == testaudio::kRate);
    CHECK(trimmed.durationMs() >= 2'400);
    CHECK(trimmed.durationMs() <= 2'400 + 2 * 160);
    // Silence is nothing; a long recording is cut at the cap.
    CHECK(VoiceSample::trimmed(audio(testaudio::quiet(3'000))).samples.isEmpty());
    CHECK(VoiceSample::trimmed(audio(testaudio::speech(20'000))).durationMs() == VoiceSample::kMaxMs);
}

TEST_CASE("A sample is usable when it is long enough and not silence; the error says what to do") {
    CHECK(VoiceSample::usable(audio(testaudio::speech(4'000))).isOk());
    const Result<void> silent = VoiceSample::usable(audio(testaudio::quiet(5'000)));
    REQUIRE_FALSE(silent);
    CHECK(silent.error().message.contains(u"microphone"_s));
    const Result<void> brief = VoiceSample::usable(audio(testaudio::speech(1'500)));
    REQUIRE_FALSE(brief);
    CHECK(brief.error().message.contains(u"Too short"_s));
}

TEST_CASE("The sample is saved 0600 with its words, loads back the same, and is forgotten whole") {
    QTemporaryDir dir;
    CHECK_FALSE(VoiceSample::load(dir.path()));
    VoiceSample sample;
    sample.audio = audio(testaudio::speech(3'500));
    sample.text = VoiceSample::passage();
    REQUIRE(sample.save(dir.path()).isOk());
    for (const QString &path : {VoiceSample::wavPath(dir.path()), VoiceSample::infoPath(dir.path())}) {
        INFO(path.toStdString());
        CHECK(QFile::exists(path));
#ifdef Q_OS_UNIX
        CHECK((QFile::permissions(path) & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup | QFileDevice::WriteOther)) == 0);
#endif
    }
    const Result<VoiceSample> loaded = VoiceSample::load(dir.path());
    REQUIRE(loaded);
    CHECK(loaded.value().audio.samples == sample.audio.samples);
    CHECK(loaded.value().audio.sampleRate == testaudio::kRate);
    CHECK(loaded.value().text == VoiceSample::passage());
    CHECK(loaded.value().recorded.isValid());
    CHECK(loaded.value().durationMs() == 3'500);
    REQUIRE(VoiceSample::forget(dir.path()).isOk());
    CHECK_FALSE(QFile::exists(VoiceSample::wavPath(dir.path())));
    CHECK_FALSE(QFile::exists(VoiceSample::infoPath(dir.path())));
    CHECK(VoiceSample::forget(dir.path()).isOk());      // nothing there: fine
}
