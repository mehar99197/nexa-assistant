// core/mission/TaskTarget: which task "task two", "the browser task" or "BROWSER-01" means.

#include "core/mission/TaskTarget.h"

#include "QtStringMakers.h"

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

Task task(TaskId id, AgentRole role, TaskState state, int depth = 0) {
    Task t;
    t.id = id;
    t.role = role;
    t.state = state;
    t.depth = depth;
    t.agentId = agentIdFor(role, int(id));
    return t;
}

} // namespace

TEST_CASE("A task is named by its number, its kind or its agent; nothing named is the one in view") {
    const QList<Task> tasks{task(1, AgentRole::Web, TaskState::Done), task(2, AgentRole::Code, TaskState::Running),
                            task(3, AgentRole::Web, TaskState::Running), task(4, AgentRole::Web, TaskState::Paused),
                            task(5, AgentRole::Code, TaskState::Running, 1)};
    CHECK(TaskTarget::resolve(u"2"_s, tasks, std::nullopt) == TaskId(2));
    CHECK(TaskTarget::resolve(u"two"_s, tasks, std::nullopt) == TaskId(2));
    CHECK(TaskTarget::resolve(u"browser"_s, tasks, std::nullopt) == TaskId(4));       // the newest web one not done
    CHECK(TaskTarget::resolve(u"web"_s, tasks, std::nullopt) == TaskId(4));
    CHECK(TaskTarget::resolve(u"code"_s, tasks, std::nullopt) == TaskId(2));          // a sub-task is not meant
    CHECK(TaskTarget::resolve(u"browser 01"_s, tasks, std::nullopt) == TaskId(1));
    CHECK(TaskTarget::resolve(u"BROWSER-03"_s, tasks, std::nullopt) == TaskId(3));
    CHECK_FALSE(TaskTarget::resolve(u"9"_s, tasks, std::nullopt).has_value());
    CHECK_FALSE(TaskTarget::resolve(u"phone"_s, tasks, std::nullopt).has_value());
    CHECK(TaskTarget::resolve({}, tasks, TaskId(3)) == TaskId(3));
    CHECK(TaskTarget::resolve({}, tasks, TaskId(1)) == TaskId(4));      // the one in view is done: the newest at work
    CHECK(TaskTarget::resolve({}, tasks, std::nullopt) == TaskId(4));
}
