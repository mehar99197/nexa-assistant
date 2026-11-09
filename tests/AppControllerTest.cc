// app/AppController against real core services (registry, policy, executor, agent, router) and a scripted model:
// what the controller makes of the tools that run and of the requests it is given. No window, no voice.

#include "app/AppController.h"
#include "core/agent/Agent.h"
#include "core/agent/AppContext.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

struct ControllerRig {
    ControllerRig()
        : store(dir.filePath(u"config.json"_s)),
          providers(&http),
          audit(dir.filePath(u"audit.jsonl"_s)),
          executor(&tools, &policy, &audit),
          router([this](const QString &tool) { return tools.contains(tool); }),
          agent(&executor, &tools) {
        tools.add(FunctionTool::sync(ToolSpec{u"run_command"_s, u"Run a command"_s, {}},
                                     [](const QJsonObject &) -> Result<ToolResult> { return ToolResult{u"ran"_s, {}}; }));
        // Never finishes until finished by hand or cancelled.
        tools.add(std::make_unique<FunctionTool>(
            ToolSpec{u"slow"_s, u"Slow"_s, {}}, [this](const QJsonObject &, ITool::Done done) { pendingSlow = std::move(done); },
            [this] {
                if (pendingSlow) std::exchange(pendingSlow, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
            }));
        controller = std::make_unique<AppController>(AppServices{.store = &store,
                                                                 .providers = &providers,
                                                                 .tools = &tools,
                                                                 .executor = &executor,
                                                                 .router = &router,
                                                                 .agent = &agent,
                                                                 .panic = &panic,
                                                                 .policy = &policy,
                                                                 .context = &context},
                                                     Config{}, u"a test machine"_s);
    }

    // Runs a tool the way the fast path and the agent do; true once it reported back.
    bool run(const QString &tool) {
        bool done = false;
        executor.execute(tool, {}, u"gui"_s, [&done](Result<ToolResult>) { done = true; });
        return QTest::qWaitFor([&done] { return done; }, kWaitMs);
    }

    QTemporaryDir dir;
    ConfigStore store;
    HttpClient http;
    ProviderRegistry providers;
    ToolRegistry tools;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
    IntentRouter router;
    Agent agent;
    PanicController panic;
    AppContext context;
    ITool::Done pendingSlow;
    std::unique_ptr<AppController> controller;
};

} // namespace

TEST_CASE("A command run while an app is in front keeps that app in front") {
    ControllerRig rig;
    rig.context.noteOpened(OpenedApp{u"GNOME Terminal"_s, u"org.gnome.Terminal.desktop"_s, u"gnome-terminal"_s,
                                     {u"System"_s, u"TerminalEmulator"_s}});
    rig.context.setTimeoutMs(600);
    QTest::qWait(400);
    REQUIRE(rig.run(u"run_command"_s));
    QTest::qWait(400);      // 800 ms since it was opened, 400 since the command
    CHECK(rig.context.isActive());
}

TEST_CASE("A refused action does not end the acting state of one still running") {
    ControllerRig rig;
    bool slowDone = false;
    rig.executor.execute(u"slow"_s, {}, u"gui"_s, [&slowDone](Result<ToolResult>) { slowDone = true; });
    REQUIRE(QTest::qWaitFor([&rig] { return rig.pendingSlow != nullptr; }, kWaitMs));
    CHECK(rig.controller->coreMode() == CoreMode::Acting);

    REQUIRE(rig.run(u"no_such_tool"_s));        // refused before it starts: finished without a start
    CHECK(rig.controller->coreMode() == CoreMode::Acting);

    std::exchange(rig.pendingSlow, nullptr)(ToolResult{u"done"_s, {}});
    REQUIRE(QTest::qWaitFor([&slowDone] { return slowDone; }, kWaitMs));
    CHECK(rig.controller->coreMode() != CoreMode::Acting);
}
