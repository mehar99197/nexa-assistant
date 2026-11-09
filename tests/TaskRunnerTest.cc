// core/mission/TaskRunner: one task's run — the fast path's tool, the model after it, and what it measures.

#include "core/audit/AuditLog.h"
#include "core/mission/TaskRunner.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using Reply = test::ScriptedProvider::Reply;

namespace {

constexpr int kWaitMs = 5'000;

struct Rig {
    Rig() : audit(dir.filePath(u"audit.jsonl"_s)), executor(&registry, &policy, &audit), runner(7, u"voice"_s, &executor, &registry) {
        registry.add(FunctionTool::sync(ToolSpec{u"echo"_s, u"Echo"_s, {}}, [](const QJsonObject &) -> Result<ToolResult> {
            return ToolResult{u"echoed\nsecond line"_s, std::nullopt};
        }));
        provider = std::make_shared<test::ScriptedProvider>();
        runner.setProvider(provider, u"m"_s);
        QObject::connect(&runner, &TaskRunner::finished, [this](FinishReason reason, bool) { finished = reason; });
        QObject::connect(&runner, &TaskRunner::failed, [this](const Error &e) { error = e; });
    }
    bool waitDone() { return QTest::qWaitFor([this] { return finished.has_value() || error.has_value(); }, kWaitMs); }

    QTemporaryDir dir;
    ToolRegistry registry;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
    TaskRunner runner;
    std::shared_ptr<test::ScriptedProvider> provider;
    std::optional<FinishReason> finished;
    std::optional<Error> error;
};

} // namespace

TEST_CASE("A spoken request is marked for the model, and the first word's time and the pace are measured") {
    Rig rig;
    rig.provider->script = {Reply{u" and the rest of the answer"_s, {}, {}, {u"First words"_s, u" keep coming"_s}}};
    QSignalSpy metrics(&rig.runner, &TaskRunner::metricsChanged);
    rig.runner.start(u"system"_s, u"what time is it"_s);
    REQUIRE(rig.waitDone());
    CHECK(rig.finished == FinishReason::Stop);
    CHECK(rig.provider->requests.first().messages.last().text == u"[voice] what time is it"_s);
    const TaskRunner::Metrics measured = rig.runner.metrics();
    CHECK(measured.firstTokenMs >= 0);
    CHECK(measured.tokensPerSecond > 0);
    CHECK(measured.steps == 1);
    CHECK(metrics.size() >= 1);
}

TEST_CASE("The fast path's tool answers, with its output shown, and the conversation keeps it") {
    Rig rig;
    QSignalSpy deltas(&rig.runner, &TaskRunner::textDelta);
    QSignalSpy route(&rig.runner, &TaskRunner::route);
    rig.runner.startFastPath(u"system"_s, u"echo it"_s, IntentMatch{.toolName = u"echo"_s});
    REQUIRE(rig.waitDone());
    CHECK(route.at(0).at(0).toString() == u"fast"_s);
    REQUIRE(deltas.size() == 1);
    CHECK(deltas.at(0).at(0).toString().contains(u"second line"_s));
    CHECK(rig.runner.conversation().messages().last().text == u"echoed\nsecond line"_s);
    CHECK(rig.provider->requests.isEmpty());
}

TEST_CASE("A fast path that misses hands the request to the model") {
    Rig rig;
    rig.provider->script = {Reply{u"I will do it another way"_s, {}, {}}};
    rig.runner.startFastPath(u"system"_s, u"open frobnicator"_s, IntentMatch{.toolName = u"no_such_tool"_s});
    REQUIRE(rig.waitDone());
    CHECK(rig.finished == FinishReason::Stop);
    REQUIRE(rig.provider->requests.size() == 1);
    CHECK(rig.provider->requests.first().messages.last().text == u"[voice] open frobnicator"_s);
}

TEST_CASE("A runner gives its provider up only while idle") {
    Rig rig;
    rig.runner.start(u"system"_s, u"hang"_s);          // empty script: the stream hangs
    CHECK(rig.runner.releaseProvider() == nullptr);
    rig.runner.stop();
    CHECK(rig.finished == FinishReason::Cancelled);
    CHECK(rig.runner.releaseProvider() != nullptr);
    CHECK_FALSE(rig.runner.hasProvider());
}
