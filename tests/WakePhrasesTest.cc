// WakePhrases: what the detectors look for, built from the name the owner chose.

#include "voice/wake/WakePhrases.h"

#include "voice/wake/WordPieces.h"

#include "QtStringMakers.h"
#include "WakeTestPieces.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("WakePhrases spells a name the ways it sounds") {
    CHECK(WakePhrases::spellings(u"Nexa"_s) == QStringList{u"nexa"_s, u"necksa"_s, u"neksa"_s});
    CHECK(WakePhrases::spellings(u"Jarvis"_s) == QStringList{u"jarvis"_s});
    CHECK(WakePhrases::spellings(u"Phoenix"_s) == QStringList{u"phoenix"_s, u"phoenicks"_s, u"phoeniks"_s, u"foenix"_s});
    CHECK(WakePhrases::spellings(u"Mister-Robot"_s) == QStringList{u"mister robot"_s});
}

TEST_CASE("WakePhrases writes the spotter's keyword lines") {
    const WordPieces pieces(testpieces::kPieces);
    const Result<QString> lines = WakePhrases::keywordLines(u"Nexa"_s, pieces);
    REQUIRE(lines.isOk());
    const QStringList list = lines.value().split(u'\n', Qt::SkipEmptyParts);
    CHECK(list.size() == 15);
    CHECK(list.contains(u"▁HE Y ▁NE X A @HEY_NEXA"_s));
    CHECK(list.contains(u"▁OKAY ▁NE X A @OKAY_NEXA"_s));
    CHECK(list.contains(u"▁NE X A @NEXA"_s));
    CHECK(list.contains(u"▁NE CK S A @NECKSA"_s));
    for (const QString &line : list) CHECK_FALSE(line.section(u'@', 1).contains(u' '));
    CHECK_FALSE(WakePhrases::keywordLines(u"N3"_s, pieces).isOk());

    CHECK(WakePhrases::isGreeted(u"OK_NEXA"_s));
    CHECK_FALSE(WakePhrases::isGreeted(u"NECKSA"_s));
}

TEST_CASE("WakePhrases tunes the detectors for the sensitivity") {
    const SpotterTuning low = WakePhrases::tuning(WakeSensitivity::Low);
    CHECK(low.boost == 1.0f);
    CHECK(low.threshold == 0.25f);
    CHECK(low.sileroThreshold == 0.6f);
    CHECK(low.startMarginDb == 16.0);
    const SpotterTuning normal = WakePhrases::tuning(WakeSensitivity::Normal);
    CHECK(normal.boost == 2.0f);
    CHECK(normal.threshold == 0.10f);
    CHECK(normal.sileroThreshold == 0.5f);
    CHECK(normal.startMarginDb == 13.0);
    const SpotterTuning high = WakePhrases::tuning(WakeSensitivity::High);
    CHECK(high.boost == 3.0f);
    CHECK(high.threshold == 0.05f);
    CHECK(high.sileroThreshold == 0.4f);
    CHECK(high.startMarginDb == 10.0);
    CHECK(WakePhrases::sensitivityFrom(u"high"_s) == WakeSensitivity::High);
    CHECK(WakePhrases::sensitivityFrom(u"loud"_s) == WakeSensitivity::Normal);
}

TEST_CASE("WakePhrases refuses names that cannot be spotted") {
    CHECK(WakePhrases::check(u"Jarvis"_s).isOk());
    CHECK(WakePhrases::check(u"Mister Robot"_s).isOk());
    const auto refused = [](const QString &name) {
        const Result<void> checked = WakePhrases::check(name);
        REQUIRE_FALSE(checked.isOk());
        CHECK(checked.error().code == ErrorCode::InvalidArgument);
        return checked.error().message;
    };
    CHECK(refused(u"N3"_s) == u"Use at least two letters."_s);
    CHECK(refused(u"نیکسا"_s) == u"Use Latin letters (a–z) so the name can be spotted instantly."_s);
    CHECK(refused(u"Abcdefghijklmnopqrstuvwxy"_s) == u"Use 2–24 characters."_s);
    CHECK(refused(u"R2D2 unit"_s) == u"Use letters, spaces, hyphens and apostrophes only."_s);
    CHECK(refused(u"one two three four"_s) == u"Use at most three words."_s);
}
