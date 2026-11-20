// The listening front: the ring of raw audio, the energy voice detector, and the front that runs the noise
// filter, the voice detector and the spotter — inline or on a worker thread.

#include "voice/listen/AudioRing.h"
#include "voice/listen/EnergyVoiceDetector.h"
#include "voice/listen/ListeningFront.h"
#include "voice/wake/IWakeSpotter.h"

#include "QtStringMakers.h"
#include "VoiceTestAudio.h"

#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <tuple>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// Hears "HEY_NEXA" at `hitAt` once the audio has passed it.
class FakeSpotter : public IWakeSpotter {
public:
    QList<Hit> feed(QByteArrayView pcm16k) override {
        seen += pcm16k.size() / 2;
        QList<Hit> hits;
        if (!fired && hitAt >= 0 && seen > hitAt) {
            fired = true;
            hits.append(Hit{.label = u"HEY_NEXA"_s, .phraseStart = hitAt, .heardAt = seen});
        }
        return hits;
    }
    void reset() override {
        seen = 0;
        fired = false;
    }

    qint64 hitAt{-1};
    qint64 seen{0};
    bool fired{false};
};

ListeningFront::Parts energyParts(std::unique_ptr<IWakeSpotter> spotter = nullptr) {
    ListeningFront::Parts parts;
    parts.detector = std::make_unique<EnergyVoiceDetector>();
    parts.spotter = std::move(spotter);
    return parts;
}

} // namespace

TEST_CASE("AudioRing keeps the last seconds by position") {
    AudioRing ring(3);
    ring.append(testaudio::quiet(2000));
    ring.append(testaudio::speech(3000));
    CHECK(ring.end() == 80000);
    CHECK(ring.begin() == 32000);
    CHECK(ring.slice(0, 40000).size() == (40000 - 32000) * 2);
    CHECK(ring.slice(40000, 40000).isEmpty());
    CHECK(std::abs(ring.levelDb(40000, 80000) - -15.0) < 1.0);
    ring.reset();
    CHECK(ring.end() == 0);
    CHECK(ring.begin() == 0);
}

TEST_CASE("EnergyVoiceDetector reports where speech starts and ends") {
    EnergyVoiceDetector detector;
    QList<IVoiceDetector::Event> events = detector.feed(testaudio::quiet(500) + testaudio::speech(600));
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == IVoiceDetector::Event::Kind::Started);
    CHECK(std::abs(events[0].start - 3200) <= 320);
    events = detector.feed(testaudio::quiet(1200));
    REQUIRE(events.size() == 1);
    CHECK(events[0].kind == IVoiceDetector::Event::Kind::Ended);
    CHECK(std::abs(events[0].start - 3200) <= 320);
    CHECK(std::abs(events[0].end - 20800) <= 320);
}

TEST_CASE("The inline front reports speech and the spotter's hits by position") {
    auto spotter = std::make_unique<FakeSpotter>();
    spotter->hitAt = 8000;
    ListeningFront front(energyParts(std::move(spotter)), ListeningFront::Threading::Inline);
    CHECK(front.hasSpotter());
    QList<qint64> started;
    QList<std::pair<qint64, qint64>> ended;
    QList<std::tuple<QString, qint64, qint64>> heard;
    QObject::connect(&front, &ListeningFront::speechStarted, &front, [&](qint64 start) { started.append(start); });
    QObject::connect(&front, &ListeningFront::speechEnded, &front,
                     [&](qint64 start, qint64 end) { ended.append({start, end}); });
    QObject::connect(&front, &ListeningFront::wakeHeard, &front, [&](const QString &label, qint64 phrase, qint64 at) {
        heard.append({label, phrase, at});
    });

    front.feed(testaudio::quiet(500) + testaudio::speech(600));
    front.feed(testaudio::quiet(1200));
    REQUIRE(started.size() == 1);
    CHECK(std::abs(started[0] - 3200) <= 320);
    REQUIRE(ended.size() == 1);
    CHECK(std::abs(ended[0].first - 3200) <= 320);
    CHECK(std::abs(ended[0].second - 20800) <= 320);
    REQUIRE(heard.size() == 1);
    CHECK(std::get<0>(heard[0]) == u"HEY_NEXA"_s);
    CHECK(std::get<1>(heard[0]) == 8000);

    front.reset();      // positions start again at 0
    front.feed(testaudio::quiet(500) + testaudio::speech(600) + testaudio::quiet(1200));
    REQUIRE(started.size() == 2);
    CHECK(std::abs(started[1] - 3200) <= 320);
}

TEST_CASE("The worker front reports the same events on the GUI thread") {
    ListeningFront front(energyParts(), ListeningFront::Threading::Worker);
    QThread *receivedOn = nullptr;
    qint64 start = -1;
    qint64 end = -1;
    QObject::connect(&front, &ListeningFront::speechEnded, &front, [&](qint64 from, qint64 to) {
        receivedOn = QThread::currentThread();
        start = from;
        end = to;
    });
    QSignalSpy ended(&front, &ListeningFront::speechEnded);
    front.feed(testaudio::quiet(500) + testaudio::speech(600));
    front.feed(testaudio::quiet(1200));
    REQUIRE(ended.wait(3000));
    CHECK(receivedOn == QThread::currentThread());
    CHECK(std::abs(start - 3200) <= 320);
    CHECK(std::abs(end - 20800) <= 320);
}

TEST_CASE("A worker front's events from before a reset are dropped") {
    ListeningFront front(energyParts(), ListeningFront::Threading::Worker);
    QSignalSpy ended(&front, &ListeningFront::speechEnded);
    front.feed(testaudio::quiet(500) + testaudio::speech(600) + testaudio::quiet(1200));
    front.reset();
    QTest::qWait(500);
    CHECK(ended.isEmpty());
}
