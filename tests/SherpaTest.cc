// sherpa-onnx's parts: the library loaded at run time, the spotter's placing of a phrase, and — where the library
// and its models are installed (scripts/download-sherpa.sh) — the spotter, Silero and the noise reducer for real.

#include "voice/listen/NoiseReducer.h"
#include "voice/listen/SileroVoiceDetector.h"
#include "voice/wake/KeywordSpotter.h"
#include "voice/wake/SherpaRuntime.h"
#include "voice/wake/WakePhrases.h"
#include "voice/wake/WordPieces.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("Without the library, the runtime says how to install it") {
    SherpaRuntime runtime(u"/nonexistent/nexa-sherpa"_s);
    CHECK_FALSE(runtime.load());
    CHECK_FALSE(runtime.available());
    CHECK(runtime.problem().contains(u"scripts/download-sherpa.sh"_s));
}

TEST_CASE("The spotter places the phrase before the moment it noticed it") {
    // Noticed at 6.0 s; the phrase's pieces span 0.68 s; the hit comes about 0.45 s after its last piece.
    CHECK(KeywordSpotter::phraseStartSample(96000, 0.96f, 1.64f) == 77920);
    CHECK(KeywordSpotter::phraseStartSample(4000, 0.0f, 0.5f) == 0);    // never before the audio began
}

TEST_CASE("Installed sherpa-onnx: word pieces from the real model", "[sherpa]") {
    SherpaRuntime runtime;
    if (!runtime.load()) SKIP("sherpa-onnx is not installed: " << runtime.problem().toStdString());
    const Result<WordPieces> pieces = WordPieces::fromModelFile(runtime.kwsDir() + u"/bpe.model"_s);
    REQUIRE(pieces.isOk());
    // As sentencepiece split them with this model (the probe of 2026-10-29).
    for (const auto &[text, expected] : QList<std::pair<QString, QString>>{
             {u"hey nexa"_s, u"▁HE Y ▁NE X A"_s},          {u"ok nexa"_s, u"▁O K ▁NE X A"_s},
             {u"hey jarvis"_s, u"▁HE Y ▁JA R VI S"_s},     {u"hey tauseeq"_s, u"▁HE Y ▁TA U SE E Q"_s},
             {u"hey ayesha"_s, u"▁HE Y ▁A Y ES HA"_s},     {u"hi google"_s, u"▁HI ▁GO O G LE"_s},
             {u"love and peace"_s, u"▁LOVE ▁AND ▁P E A CE"_s}, {u"happy new year"_s, u"▁HA PP Y ▁NEW ▁YEAR"_s}}) {
        INFO(text.toStdString());
        CHECK(pieces.value().split(text).join(u' ') == expected);
    }
}

TEST_CASE("Installed sherpa-onnx: the spotter, Silero and the noise reducer run", "[sherpa]") {
    SherpaRuntime runtime;
    if (!runtime.load()) SKIP("sherpa-onnx is not installed: " << runtime.problem().toStdString());
    const Result<WordPieces> pieces = WordPieces::fromModelFile(runtime.kwsDir() + u"/bpe.model"_s);
    REQUIRE(pieces.isOk());
    const Result<QString> lines = WakePhrases::keywordLines(u"Nexa"_s, pieces.value());
    REQUIRE(lines.isOk());

    Result<std::unique_ptr<KeywordSpotter>> spotter =
        KeywordSpotter::create(&runtime, lines.value(), WakePhrases::tuning(WakeSensitivity::Normal));
    REQUIRE(spotter.isOk());
    CHECK(spotter.value()->feed(testaudio::quiet(2000)).isEmpty());

    Result<std::unique_ptr<SileroVoiceDetector>> silero = SileroVoiceDetector::create(&runtime, 0.5f);
    REQUIRE(silero.isOk());
    // A steady tone is loud, but it is not a voice.
    CHECK(silero.value()->feed(testaudio::quiet(1000) + testaudio::speech(2000) + testaudio::quiet(1000)).isEmpty());

    Result<std::unique_ptr<NoiseReducer>> reducer = NoiseReducer::create(&runtime);
    REQUIRE(reducer.isOk());
    QByteArray cleaned;
    for (std::uint32_t i = 0; i < 20; ++i) cleaned += reducer.value()->process(testaudio::speech(100, i + 1));
    CHECK(std::abs(cleaned.size() / 2 - 32000) <= 512);
}
