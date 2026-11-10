// core/mission/HardcoreMode: when the laptop's keyboard and mouse are locked. Locked while a hardcore task is active
// (queued, running, waiting for the owner or for its sub-task); given back when none is, when the owner says unlock,
// and when the lock is lost — and never left held because a task ended at an awkward moment.

#include "core/mission/HardcoreMode.h"

#include "FakeInputLock.h"
#include "MissionRig.h"
#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace nexa::test;
using namespace Qt::StringLiterals;
using State = HardcoreMode::State;

namespace {

struct Rig {
    Rig() : hardcore(board.mission.get(), &lock) { hardcore.setCountdownMs(30); }

    TaskId hardTask(const QString &text = u"tidy the downloads folder"_s) {
        TaskRequest request;
        request.text = text;
        request.hardcore = true;
        return board.mission->add(request);
    }
    bool waitForState(State wanted) {
        return QTest::qWaitFor([this, wanted] { return hardcore.state() == wanted; }, kWaitMs);
    }
    void finish(TaskId id, int provider = 0) {
        REQUIRE(provider < board.env.providers.size());
        board.env.providers[provider]->lastStream->play(u"done"_s, {}, std::nullopt);
        REQUIRE(board.waitFor(id, TaskState::Done));
    }

    Board board;
    FakeLock lock;
    HardcoreMode hardcore;
};

} // namespace

TEST_CASE("The switch on and a hardcore task: a countdown, then the keyboard and mouse are locked") {
    Rig rig;
    rig.hardcore.setSwitch(true);
    CHECK(rig.hardcore.state() == State::Armed);
    CHECK(rig.hardcore.banner().contains(u"next task"_s));
    const TaskId id = rig.hardTask();
    CHECK(rig.hardcore.state() == State::Counting);
    CHECK(rig.lock.locks == 0);     // not before the countdown
    CHECK(rig.hardcore.banner().contains(u"tidy the downloads folder"_s));
    REQUIRE(rig.waitForState(State::Locked));
    CHECK(rig.lock.locks == 1);
    CHECK(rig.hardcore.devices().contains(u"SynPS/2 Synaptics TouchPad"_s));
    CHECK(rig.hardcore.banner().contains(u"locked"_s));

    rig.finish(id);
    REQUIRE(rig.waitForState(State::Armed));
    CHECK(rig.lock.unlocks == 1);
    CHECK_FALSE(rig.lock.held);
    CHECK(rig.hardcore.isSwitchOn());       // the next task locks again
}

TEST_CASE("A task that is not hardcore never locks anything") {
    Rig rig;
    rig.board.ask(u"tell me a joke"_s);
    QTest::qWait(120);
    CHECK(rig.hardcore.state() == State::Off);
    CHECK(rig.lock.locks == 0);
}

TEST_CASE("A hardcore task waiting in the queue keeps the lock between two tasks") {
    Rig rig;
    MissionConfig oneSlot;
    oneSlot.maxConcurrent = 1;
    rig.board.mission->setConfig(oneSlot);
    const TaskId first = rig.hardTask(u"first"_s);
    const TaskId second = rig.hardTask(u"second"_s);
    CHECK(rig.board.state(second) == TaskState::Queued);
    REQUIRE(rig.waitForState(State::Locked));
    rig.finish(first, 0);
    REQUIRE(rig.board.waitFor(second, TaskState::Running));
    CHECK(rig.hardcore.state() == State::Locked);
    CHECK(rig.lock.unlocks == 0);
    rig.finish(second, 1);
    REQUIRE(rig.waitForState(State::Off));
    CHECK(rig.lock.unlocks == 1);
}

TEST_CASE("A paused task gives the keyboard back, and takes it again when it goes on") {
    Rig rig;
    const TaskId id = rig.hardTask();
    REQUIRE(rig.waitForState(State::Locked));
    rig.board.mission->pause(id);
    REQUIRE(rig.waitForState(State::Off));
    CHECK(rig.lock.unlocks == 1);
    rig.board.mission->resume(id);
    CHECK(rig.hardcore.state() == State::Counting);
    REQUIRE(rig.waitForState(State::Locked));
    CHECK(rig.lock.locks == 2);
}

TEST_CASE("A task that ends during the countdown never locks") {
    Rig rig;
    rig.hardcore.setCountdownMs(2'000);
    const TaskId id = rig.hardTask();
    CHECK(rig.hardcore.state() == State::Counting);
    rig.finish(id);
    REQUIRE(rig.waitForState(State::Off));
    QTest::qWait(50);
    CHECK(rig.lock.locks == 0);
    CHECK(rig.lock.unlocks == 0);
}

TEST_CASE("A task that ends while the helper is being asked does not leave the laptop locked") {
    Rig rig;
    rig.lock.holdReply = true;      // e.g. the password prompt is up
    const TaskId id = rig.hardTask();
    REQUIRE(rig.waitForState(State::Locking));
    rig.finish(id);
    rig.lock.answer();
    CHECK_FALSE(rig.lock.held);
    CHECK(rig.lock.unlocks == 1);
    CHECK(rig.hardcore.state() == State::Off);
}

TEST_CASE("Unlock: the keyboard comes back at once, the task goes on, and hardcore mode is off") {
    Rig rig;
    rig.hardcore.setSwitch(true);
    const TaskId id = rig.hardTask();
    REQUIRE(rig.waitForState(State::Locked));
    QString said;
    QObject::connect(&rig.hardcore, &HardcoreMode::unlocked, [&said](const QString &why) { said = why; });
    rig.hardcore.unlockNow(u"you said unlock"_s);
    CHECK(rig.hardcore.state() == State::Off);
    CHECK_FALSE(rig.hardcore.isSwitchOn());
    CHECK_FALSE(rig.lock.held);
    CHECK(said == u"you said unlock"_s);
    CHECK(rig.board.state(id) == TaskState::Running);
    CHECK_FALSE(rig.board.mission->task(id)->hardcore);
    QTest::qWait(80);
    CHECK(rig.lock.locks == 1);     // and it does not lock again behind the owner's back
}

TEST_CASE("A lost lock is given up the same way, and says why") {
    Rig rig;
    rig.hardTask();
    REQUIRE(rig.waitForState(State::Locked));
    QString said;
    QObject::connect(&rig.hardcore, &HardcoreMode::unlocked, [&said](const QString &why) { said = why; });
    emit rig.lock.lost(u"Nexa stopped answering for 30 seconds."_s);
    CHECK(rig.hardcore.state() == State::Off);
    CHECK(said == u"Nexa stopped answering for 30 seconds."_s);
    QTest::qWait(80);
    CHECK(rig.lock.locks == 1);
}

TEST_CASE("A refused lock lets the task run, and the banner says why") {
    Rig rig;
    rig.lock.refuse = Error{ErrorCode::PermissionDenied, u"Permission was refused."_s};
    const TaskId id = rig.hardTask();
    REQUIRE(rig.waitForState(State::Failed));
    CHECK(rig.hardcore.reason() == u"Permission was refused."_s);
    CHECK(rig.hardcore.banner().contains(u"Permission was refused."_s));
    CHECK(rig.board.state(id) == TaskState::Running);
    rig.finish(id);
    REQUIRE(rig.waitForState(State::Off));
}

TEST_CASE("Switching hardcore off during the countdown cancels the lock") {
    Rig rig;
    rig.hardcore.setCountdownMs(2'000);
    rig.hardcore.setSwitch(true);
    rig.hardTask();
    CHECK(rig.hardcore.state() == State::Counting);
    rig.hardcore.setSwitch(false);
    CHECK(rig.hardcore.state() == State::Off);
    QTest::qWait(50);
    CHECK(rig.lock.locks == 0);
}

TEST_CASE("A sub-task of a hardcore task is hardcore too") {
    Rig rig;
    const TaskId lead = rig.hardTask(u"plan the release"_s);
    TaskId child = 0;
    QObject::connect(rig.board.mission.get(), &MissionControl::taskAdded, [&child](quint64 id) { child = id; });
    rig.board.mission->delegate(lead, AgentRole::Code, u"write the changelog"_s, {}, [](Result<ToolResult>) {});
    REQUIRE(child != 0);
    CHECK(rig.board.mission->task(child)->hardcore);
    CHECK(HardcoreMode::wantsLock(rig.board.mission->tasks()));
}

TEST_CASE("wantsLock: active hardcore tasks only") {
    Task running;
    running.hardcore = true;
    running.state = TaskState::Running;
    Task paused = running;
    paused.state = TaskState::Paused;
    Task done = running;
    done.state = TaskState::Done;
    Task awaiting = running;
    awaiting.state = TaskState::AwaitingYou;
    Task ordinary;
    ordinary.state = TaskState::Running;
    CHECK(HardcoreMode::wantsLock({running}));
    CHECK(HardcoreMode::wantsLock({awaiting}));     // the owner answers by voice or phone: the lock holds
    CHECK_FALSE(HardcoreMode::wantsLock({paused, done, ordinary}));
}
