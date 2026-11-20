// core/mission/Task: a task's title, its state's words, and the progress shown for it.

#include "core/mission/Task.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

TEST_CASE("A task's title is its first line, shortened") {
    CHECK(titleFor(u"  Register semester courses\nthe ones in my notes"_s) == u"Register semester courses"_s);
    CHECK(titleFor(u"\n\n  open   calculator  "_s) == u"open calculator"_s);
    const QString longText = QString(100, u'x');
    CHECK(titleFor(longText).size() == 80);
    CHECK(titleFor(longText).endsWith(u"…"_s));
}

TEST_CASE("Every state has words, and only running and awaiting hold a slot") {
    CHECK(taskStateName(TaskState::AwaitingYou) == u"AWAITING YOU"_s);
    CHECK(taskStateName(TaskState::Running) == u"RUNNING"_s);
    for (const TaskState state : {TaskState::Queued, TaskState::Running, TaskState::AwaitingYou, TaskState::Delegating,
                                  TaskState::Paused, TaskState::Done, TaskState::Failed, TaskState::Stopped}) {
        CHECK_FALSE(taskStateName(state).isEmpty());
        CHECK(holdsSlot(state) == (state == TaskState::Running || state == TaskState::AwaitingYou));
        CHECK(isFinished(state) == (state == TaskState::Done || state == TaskState::Failed || state == TaskState::Stopped));
    }
}

TEST_CASE("Progress grows with the work, stays below 100 until done") {
    CHECK(estimateProgress(0, 0, TaskState::Queued) == 0);
    int last = 0;
    for (int steps = 1; steps < 30; ++steps) {
        const int now = estimateProgress(steps, steps * 2, TaskState::Running);
        CHECK(now >= last);
        CHECK(now < 100);
        last = now;
    }
    CHECK(estimateProgress(1, 0, TaskState::Done) == 100);
}
