// MicrophoneCheck: when the microphone's sound is clipped or drowned in noise, Nexa says so — and Lower it
// steps the input volume down until nothing clips.

#include "voice/capture/MicrophoneCheck.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// `clipped` of every 100 samples at full scale, the rest silent.
QByteArray clipping(int ms, int clipped) {
    QByteArray pcm(ms * 32, Qt::Uninitialized);
    auto *samples = reinterpret_cast<qint16 *>(pcm.data());
    for (int i = 0; i < ms * 16; ++i) samples[i] = qint16(i % 100 < clipped ? (i % 200 < 100 ? 32767 : -32768) : 0);
    return pcm;
}

// Feeds `pcm` in 100 ms chunks, the clock running from `fromMs`; gives the clock after it.
qint64 feed(MicrophoneCheck &check, const QByteArray &pcm, qint64 fromMs = 0) {
    qint64 now = fromMs;
    for (qsizetype offset = 0; offset < pcm.size(); offset += 3200) {
        now += 100;
        check.feed(QByteArrayView(pcm).mid(offset, 3200), now);
    }
    return now;
}

} // namespace

TEST_CASE("Speech in a quiet room does not overload the microphone") {
    MicrophoneCheck check;
    QByteArray room;
    for (int i = 0; i < 6; ++i) room += testaudio::quiet(700) + testaudio::speech(800);
    const qint64 now = feed(check, room);
    CHECK_FALSE(check.overloaded());
    CHECK_FALSE(check.notice(now));
}

TEST_CASE("Clipped sound overloads the microphone, and Nexa says how much") {
    MicrophoneCheck check;
    const qint64 now = feed(check, clipping(3000, 52));
    CHECK(check.overloaded());
    const std::optional<QString> notice = check.notice(now);
    REQUIRE(notice);
    CHECK(notice->contains(u"52 %"_s));
    CHECK(notice->contains(u"GNOME Settings → Sound → Input"_s));
}

TEST_CASE("A loud floor for 10 s overloads the microphone; for 9 s it does not") {
    const QByteArray loud = testaudio::make(10'000, 0.0, 0.17, 5);       // noise around -20 dBFS
    MicrophoneCheck nine;
    feed(nine, loud.left(9'000 * 32));
    CHECK_FALSE(nine.overloaded());
    MicrophoneCheck ten;
    feed(ten, loud);
    CHECK(ten.overloaded());
}

TEST_CASE("The overload notice comes at most once a minute") {
    MicrophoneCheck check;
    const qint64 now = feed(check, clipping(3000, 52));
    CHECK(check.notice(now));
    CHECK_FALSE(check.notice(now + 30'000));
    CHECK(check.notice(now + 61'000));
}

TEST_CASE("Lower it steps the input down until nothing clips and a voice has room") {
    using Stats = MicrophoneCheck::Stats;
    CHECK(MicrophoneCheck::nextVolume(100, Stats{.clippedShare = 0.3, .floorDb = -10, .p90Db = -2}) == 95);
    CHECK(MicrophoneCheck::nextVolume(20, Stats{.clippedShare = 0.0, .floorDb = -45, .p90Db = -18}) == 15);
    CHECK(MicrophoneCheck::nextVolume(20, Stats{.clippedShare = 0.0, .floorDb = -45, .p90Db = -22}) == std::nullopt);
    CHECK(MicrophoneCheck::nextVolume(5, Stats{.clippedShare = 0.1, .floorDb = -20, .p90Db = -3}) == std::nullopt);

    const Stats clipped = MicrophoneCheck::measure(clipping(2000, 52));
    CHECK(clipped.clippedShare > 0.5);
    CHECK(clipped.clippedShare < 0.54);
    const Stats quiet = MicrophoneCheck::measure(testaudio::quiet(2000));
    CHECK(quiet.clippedShare == 0.0);
    CHECK(quiet.p90Db < -50.0);
}
