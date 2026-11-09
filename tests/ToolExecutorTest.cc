// core/tools/ToolExecutor with a task on every action: the signals and the audit line carry it, a question waiting
// for the user is tied to it, and cancelling one task's tools leaves every other task's running.

#include "core/audit/AuditLog.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// A tool that finishes only when told to; every invocation waits in `calls`, and cancel() counts.
struct Waiting {
    QList<ITool::Done> calls;
    int cancels{0};
    std::unique_ptr<ITool> make(const QString &name) {
        return std::make_unique<FunctionTool>(
            ToolSpec{name, name, {}}, [this](const QJsonObject &, ITool::Done done) { calls.append(std::move(done)); },
            [this] {
                ++cancels;
                const QList<ITool::Done> all = std::exchange(calls, {});
                for (const ITool::Done &done : all) done(Error{ErrorCode::Cancelled, u"Cancelled"_s});
            });
    }
};

struct Rig {
    Rig() : audit(dir.filePath(u"audit.jsonl"_s)), executor(&registry, &policy, &audit) {
        registry.add(FunctionTool::sync(ToolSpec{u"echo"_s, u"Echo"_s, {}},
                                        [](const QJsonObject &) -> Result<ToolResult> { return ToolResult{u"echoed"_s, {}}; }));
    }
    QList<QJsonObject> auditLines() const {
        QList<QJsonObject> lines;
        QFile file(dir.filePath(u"audit.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return lines;
        for (const QByteArray &line : file.readAll().split('\n')) {
            if (!line.isEmpty()) lines.append(QJsonDocument::fromJson(line).object());
        }
        return lines;
    }

    QTemporaryDir dir;
    ToolRegistry registry;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
};

// What one execute() reported, and how often.
struct Outcome {
    int calls{0};
    std::optional<Result<ToolResult>> result;
    ITool::Done done() {
        return [this](Result<ToolResult> r) {
            ++calls;
            result = std::move(r);
        };
    }
    bool cancelled() const { return result && !result->isOk() && result->error().code == ErrorCode::Cancelled; }
};

} // namespace

TEST_CASE("An action carries its task on both signals and into the audit line") {
    Rig rig;
    QSignalSpy started(&rig.executor, &ToolExecutor::actionStarted);
    QSignalSpy finished(&rig.executor, &ToolExecutor::actionFinished);
    Outcome outcome;
    rig.executor.execute(u"echo"_s, {}, ToolContext{u"voice"_s, 7}, outcome.done());
    REQUIRE(outcome.calls == 1);
    REQUIRE(started.size() == 1);
    CHECK(started.at(0).at(3).toString() == u"voice"_s);
    CHECK(started.at(0).at(4).toULongLong() == 7);
    REQUIRE(finished.size() == 1);
    CHECK(finished.at(0).at(4).toULongLong() == 7);

    rig.executor.execute(u"echo"_s, {}, u"gui"_s, Outcome{}.done());      // no task
    const QList<QJsonObject> lines = rig.auditLines();
    REQUIRE(lines.size() == 2);
    CHECK(lines.at(0).value(u"task"_s).toInteger() == 7);
    CHECK_FALSE(lines.at(1).contains(u"task"_s));
}

TEST_CASE("Cancelling a task stops its tools and no other task's") {
    Rig rig;
    Waiting a;
    Waiting b;
    rig.registry.add(a.make(u"slow_a"_s));
    rig.registry.add(b.make(u"slow_b"_s));
    Outcome seven;
    Outcome eight;
    rig.executor.execute(u"slow_a"_s, {}, ToolContext{u"gui"_s, 7}, seven.done());
    rig.executor.execute(u"slow_b"_s, {}, ToolContext{u"gui"_s, 8}, eight.done());
    REQUIRE(a.calls.size() == 1);
    REQUIRE(b.calls.size() == 1);

    rig.executor.cancelTask(7);
    CHECK(seven.cancelled());
    CHECK(a.cancels == 1);
    CHECK(b.cancels == 0);
    CHECK(eight.calls == 0);

    std::exchange(b.calls, {}).first()(ToolResult{u"done"_s, {}});
    REQUIRE(eight.calls == 1);
    CHECK(eight.result->isOk());
}

// ITool::cancel() is per tool, not per invocation: with another task in the same tool, the cancelled task is told at
// once and its late result is dropped (still audited: it ran), while the tool goes on for the other.
TEST_CASE("Two tasks in the same tool: cancelling one tells it alone and leaves the tool running") {
    Rig rig;
    Waiting shared;
    rig.registry.add(shared.make(u"slow"_s));
    QSignalSpy finished(&rig.executor, &ToolExecutor::actionFinished);
    Outcome seven;
    Outcome eight;
    rig.executor.execute(u"slow"_s, {}, ToolContext{u"gui"_s, 7}, seven.done());
    rig.executor.execute(u"slow"_s, {}, ToolContext{u"gui"_s, 8}, eight.done());
    REQUIRE(shared.calls.size() == 2);

    rig.executor.cancelTask(7);
    CHECK(seven.cancelled());
    CHECK(shared.cancels == 0);
    CHECK(eight.calls == 0);
    REQUIRE(finished.size() == 1);
    CHECK(finished.at(0).at(4).toULongLong() == 7);

    const QList<ITool::Done> late = std::exchange(shared.calls, {});
    late.at(0)(ToolResult{u"too late"_s, {}});      // task 7's: dropped
    late.at(1)(ToolResult{u"for eight"_s, {}});
    CHECK(seven.calls == 1);
    REQUIRE(eight.calls == 1);
    CHECK(eight.result->value().text == u"for eight"_s);
    CHECK(finished.size() == 2);
    CHECK(rig.auditLines().size() == 2);
}

TEST_CASE("A question still open is withdrawn when its task is cancelled") {
    Rig rig;
    rig.policy.setMode(PolicyMode::ConfirmAll);
    std::optional<ConfirmRequest> asked;
    std::function<void(bool)> answer;
    rig.executor.setConfirmer([&](const ConfirmRequest &request, std::function<void(bool)> reply) {
        asked = request;
        answer = std::move(reply);
    });
    QSignalSpy finished(&rig.executor, &ToolExecutor::actionFinished);
    Outcome outcome;
    rig.executor.execute(u"echo"_s, {{u"msg"_s, u"hi"_s}}, ToolContext{u"phone"_s, 9}, outcome.done());
    REQUIRE(asked);
    CHECK(asked->tool == u"echo"_s);
    CHECK(asked->task == 9);
    CHECK(asked->source == u"phone"_s);
    CHECK(asked->arguments.value(u"msg"_s).toString() == u"hi"_s);
    CHECK(asked->question.startsWith(u"Allow echo?"_s));
    CHECK(outcome.calls == 0);

    rig.executor.cancelTask(9);
    CHECK(outcome.cancelled());
    REQUIRE(finished.size() == 1);
    answer(true);                   // the dialog answered after all: nothing runs, nothing is reported twice
    CHECK(outcome.calls == 1);
    CHECK(finished.size() == 1);
    CHECK(rig.auditLines().isEmpty());
}

TEST_CASE("Cancelling a task with nothing running, or task 0, does nothing") {
    Rig rig;
    Waiting w;
    rig.registry.add(w.make(u"slow"_s));
    Outcome untasked;
    rig.executor.execute(u"slow"_s, {}, u"gui"_s, untasked.done());
    rig.executor.cancelTask(0);
    rig.executor.cancelTask(42);
    CHECK(untasked.calls == 0);
    CHECK(w.cancels == 0);
    rig.executor.cancelAll();
    CHECK(untasked.cancelled());
}
