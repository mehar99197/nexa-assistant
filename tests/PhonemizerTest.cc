// voice/tts/Phonemizer: words → the phoneme tokens ZipVoice reads, through Piper's phonemizer — its JSON parsed, and,
// where Piper is installed (scripts/download-piper.sh), the program run for real.

#include "voice/VoiceBackends.h"
#include "voice/tts/Phonemizer.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("One line of piper_phonemize's JSON becomes the tokens, one per sound, spaces and marks kept") {
    // As piper_phonemize 1.2 printed "Nexa." on 2026-11-12.
    const QByteArray line = R"({"phoneme_ids":[1,0,26,0,120,0,61,0,23,0,31,0,59,0,10,0,2],"phonemes":["n","ˈ","ɛ","k","s","ə","."],"processed_text":"Nexa.","text":"Nexa."})";
    const Result<QStringList> tokens = Phonemizer::parse(line);
    REQUIRE(tokens);
    CHECK(tokens.value() == QStringList{u"n"_s, u"ˈ"_s, u"ɛ"_s, u"k"_s, u"s"_s, u"ə"_s, u"."_s});
    CHECK_FALSE(Phonemizer::parse("not json"));
    CHECK_FALSE(Phonemizer::parse(R"({"text":"x"})"));
    CHECK_FALSE(Phonemizer::parse(R"({"phonemes":[]})"));
}

TEST_CASE("Without Piper the phonemizer says what to install") {
    const Phonemizer missing(u"/nonexistent/nexa-piper"_s);
    CHECK_FALSE(missing.available());
    CHECK(missing.problem().contains(u"scripts/download-piper.sh"_s));
    CHECK_FALSE(missing.tokens(u"hello"_s));
}

TEST_CASE("Installed Piper: Nexa's name and a sentence come out as sounds", "[piper]") {
    const Phonemizer phonemizer(VoiceBackends::piperDir());
    if (!phonemizer.available()) SKIP("Piper is not installed: " << phonemizer.problem().toStdString());
    const Result<QStringList> name = phonemizer.tokens(u"Nexa"_s);
    REQUIRE(name);
    CHECK(name.value().contains(u"n"_s));
    CHECK(name.value().contains(u"ɛ"_s));
    CHECK(name.value().contains(u"ə"_s));
    const Result<QStringList> sentence = phonemizer.tokens(u"Hello world,\nthis is a test."_s);
    REQUIRE(sentence);
    CHECK(sentence.value().contains(u" "_s));
    CHECK(sentence.value().contains(u","_s));
    CHECK(sentence.value().last() == u"."_s);
    CHECK_FALSE(phonemizer.tokens(u"   "_s));
}
