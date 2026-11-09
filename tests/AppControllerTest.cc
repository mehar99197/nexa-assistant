// app/AppController against real core services (registry, policy, executor, agent, router) and a scripted model:
// what the controller makes of the tools that run and of the requests it is given. No window, no voice.

#include "app/AppController.h"
#include "core/agent/AppContext.h"
#include "core/ai/ProviderRegistry.h"
#include "core/audit/AuditLog.h"
#include "core/config/ConfigStore.h"
#include "core/intent/IntentRouter.h"
#include "core/intent/MissionIntents.h"
#include "core/intent/PcIntents.h"
#include "core/net/HttpClient.h"
#include "core/policy/PanicController.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QPointer>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

Config withScriptedModel() {
    Config config;
    config.providers = {ProviderConfig{.id = u"model"_s, .kind = u"scripted"_s, .name = u"Model"_s, .requiresKey = false}};
    config.activeProvider = u"model"_s;
    return config;
}

struct ControllerRig {
    ControllerRig()
        : store(dir.filePath(u"config.json"_s)),
          providers(&http),
          audit(dir.filePath(u"audit.jsonl"_s)),
          executor(&tools, &policy, &audit),
          router([this](const QString &tool) { return tools.contains(tool); }) {
        tools.add(FunctionTool::sync(ToolSpec{u"run_command"_s, u"Run a command"_s, {}},
                                     [](const QJsonObject &) -> Result<ToolResult> { return ToolResult{u"ran"_s, {}}; }));
        // Never finishes until finished by hand or cancelled.
        tools.add(std::make_unique<FunctionTool>(
            ToolSpec{u"slow"_s, u"Slow"_s, {}}, [this](const QJsonObject &, ITool::Done done) { pendingSlow = std::move(done); },
            [this] {
                if (pendingSlow) std::exchange(pendingSlow, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
            }));
        router.add(std::make_unique<ControlIntent>());
        router.add(std::make_unique<TaskIntent>());
        router.add(std::make_unique<ScreenIntent>());
        // A model kind of the test's own: every instance the app makes takes the next script.
        providers.registerKind(u"scripted"_s, [this](const ProviderConfig &, HttpClient *) {
            auto provider = std::make_unique<test::ScriptedProvider>();
            if (!scripts.isEmpty()) provider->script = scripts.takeFirst();
            made.append(provider.get());
            return std::unique_ptr<IAiProvider>(std::move(provider));
        });
        controller = std::make_unique<AppController>(AppServices{.store = &store,
                                                                 .providers = &providers,
                                                                 .tools = &tools,
                                                                 .executor = &executor,
                                                                 .router = &router,
                                                                 .panic = &panic,
                                                                 .policy = &policy,
                                                                 .context = &context},
                                                     withScriptedModel(), u"a test machine"_s);
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
    PanicController panic;
    AppContext context;
    ITool::Done pendingSlow;
    QList<QList<test::ScriptedProvider::Reply>> scripts;      // for the model instances, in the order they are made
    QList<QPointer<test::ScriptedProvider>> made;
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

namespace {

using Reply = test::ScriptedProvider::Reply;

// The conversation's signals, in the order they came.
struct Feed {
    explicit Feed(AppController *controller) {
        QObject::connect(controller, &AppController::userMessageAccepted, &context,
                         [this](const QString &text, const QString &) { log.append(u"you: "_s + text); });
        QObject::connect(controller, &AppController::replyStarted, &context, [this] { log.append(u"start"_s); });
        QObject::connect(controller, &AppController::replyDelta, &context,
                         [this](const QString &delta) { log.append(u"say: "_s + delta); });
        QObject::connect(controller, &AppController::replyNotice, &context,
                         [this](const QString &notice) { log.append(u"note: "_s + notice); });
        QObject::connect(controller, &AppController::replyFinished, &context, [this] { log.append(u"end"_s); });
        QObject::connect(controller, &AppController::replyFailed, &context,
                         [this](const QString &message) { log.append(u"failed: "_s + message); });
    }
    QObject context;
    QStringList log;
};

} // namespace

TEST_CASE("Two requests in a row both run, and the second carries on the conversation") {
    ControllerRig rig;
    // The model list's instance, the warm spare (the first task's), then one for the task asked while it runs.
    rig.scripts = {{}, {Reply{u"Paris"_s, {}, {}}}, {}};
    rig.controller->start();
    const quint64 first = rig.controller->submit(u"capital of France?"_s);
    REQUIRE(first != 0);
    REQUIRE(QTest::qWaitFor([&rig, first] { return rig.controller->mission()->task(first)->state == TaskState::Done; }, kWaitMs));
    CHECK_FALSE(rig.controller->isBusy());

    rig.made.at(1)->script = {Reply{u"About two million"_s, {}, {}}};
    const quint64 second = rig.controller->submit(u"and its population?"_s);
    REQUIRE(QTest::qWaitFor([&rig, second] { return rig.controller->mission()->task(second)->state == TaskState::Done; }, kWaitMs));
    // The same warm instance answered both, and saw the first exchange.
    REQUIRE(rig.made.at(1)->requests.size() == 2);
    const QList<ChatMessage> messages = rig.made.at(1)->requests.at(1).messages;
    CHECK(std::any_of(messages.cbegin(), messages.cend(), [](const ChatMessage &m) { return m.text == u"Paris"_s; }));
}

TEST_CASE("A reply still running goes on in the background, and its answer comes to the chat when it is done") {
    ControllerRig rig;
    rig.scripts = {{}, {}, {Reply{u"It is noon."_s, {}, {}}}};       // the first task hangs until told
    rig.controller->start();
    Feed feed(rig.controller.get());
    const quint64 slow = rig.controller->submit(u"write me a long report"_s);
    REQUIRE(QTest::qWaitFor([&rig] { return rig.made.size() >= 2 && rig.made.at(1)->lastStream; }, kWaitMs));
    const quint64 quick = rig.controller->submit(u"what time is it?"_s);
    REQUIRE(quick != 0);
    REQUIRE(QTest::qWaitFor([&rig, quick] { return rig.controller->mission()->task(quick)->state == TaskState::Done; }, kWaitMs));
    CHECK(rig.controller->mission()->task(slow)->state == TaskState::Running);
    CHECK(rig.controller->isBusy());

    // The slow one ends: its answer is added as a reply of its own.
    rig.made.at(1)->lastStream->play(u"Here is the report."_s, {}, std::nullopt);
    REQUIRE(QTest::qWaitFor([&rig, slow] { return rig.controller->mission()->task(slow)->state == TaskState::Done; }, kWaitMs));
    const QString agentId = rig.controller->mission()->task(slow)->agentId;
    const QStringList expected{u"you: write me a long report"_s, u"start"_s,
                               u"note: %1 goes on in the background; its answer comes here when it is done."_s.arg(agentId),
                               u"end"_s, u"you: what time is it?"_s, u"start"_s, u"say: It is noon."_s, u"end"_s,
                               u"start"_s};
    REQUIRE(feed.log.size() == expected.size() + 2);
    CHECK(feed.log.mid(0, expected.size()) == expected);
    CHECK(feed.log.at(expected.size()).contains(u"Here is the report."_s));
    CHECK(feed.log.last() == u"end"_s);
    CHECK_FALSE(rig.controller->isBusy());
}

TEST_CASE("Sharing the screen is answered as a task, and Esc stops the reply in view only") {
    ControllerRig rig;
    rig.scripts = {{}, {}, {}};
    rig.controller->start();
    const quint64 shared = rig.controller->submit(u"share my screen"_s);
    REQUIRE(shared != 0);
    CHECK(rig.controller->isScreenShared());
    CHECK(rig.controller->mission()->task(shared)->state == TaskState::Done);

    TaskRequest background;
    background.text = u"keep thinking"_s;
    const quint64 other = rig.controller->mission()->add(background);    // on the board, not in the chat
    const quint64 shown = rig.controller->submit(u"think about this"_s);
    REQUIRE(QTest::qWaitFor([&rig] { return rig.controller->isReplying(); }, kWaitMs));
    rig.controller->cancel();
    CHECK(rig.controller->mission()->task(shown)->state == TaskState::Stopped);
    CHECK(rig.controller->mission()->task(other)->state == TaskState::Running);
    rig.controller->panic();
    CHECK(rig.controller->mission()->task(other)->state == TaskState::Stopped);
}

TEST_CASE("Spoken or typed commands act on the board: stop one task, pause by number, approve only what waits") {
    ControllerRig rig;
    rig.scripts = {{}, {}, {}, {}, {}};     // every model hangs until told
    rig.controller->start();
    TaskRequest background;
    background.text = u"keep thinking"_s;
    const quint64 other = rig.controller->mission()->add(background);
    const quint64 shown = rig.controller->submit(u"think about this"_s);
    REQUIRE(QTest::qWaitFor([&rig] { return rig.controller->isReplying(); }, kWaitMs));

    CHECK(rig.controller->submit(u"be quiet"_s) == 0);       // the voice only: nothing stops
    CHECK(rig.controller->mission()->task(shown)->state == TaskState::Running);

    CHECK(rig.controller->submit(u"pause task %1"_s.arg(other)) == 0);
    CHECK(rig.controller->mission()->task(other)->state == TaskState::Paused);
    CHECK(rig.controller->submit(u"stop"_s) == 0);            // the task in view
    CHECK(rig.controller->mission()->task(shown)->state == TaskState::Stopped);
    CHECK(rig.controller->mission()->task(other)->state == TaskState::Paused);

    // "yes" with no question waiting is an answer for the model, as a request of its own.
    const quint64 answer = rig.controller->submit(u"yes"_s);
    CHECK(answer != 0);
    QSignalSpy notices(rig.controller.get(), &AppController::replyNotice);
    CHECK(rig.controller->submit(u"what are you doing"_s) == 0);
    REQUIRE_FALSE(notices.isEmpty());
    CHECK(notices.last().at(0).toString().contains(u"running"_s));
    const qsizetype before = rig.controller->mission()->tasks().size();
    CHECK(rig.controller->submit(u"add a task: write the release notes and send them"_s) == 0);
    CHECK(rig.controller->mission()->tasks().size() == before + 1);
    CHECK(rig.controller->mission()->tasks().last().text == u"write the release notes and send them"_s);
    rig.controller->panic();
}

TEST_CASE("Yes approves the question a task waits on") {
    ControllerRig rig;
    rig.policy.setMode(PolicyMode::ConfirmAll);
    rig.scripts = {{}, {Reply{{}, {ToolCall{u"c1"_s, u"run_command"_s, u"{}"_s}}, {}}, Reply{u"done"_s, {}, {}}}};
    rig.controller->start();
    const quint64 id = rig.controller->submit(u"run something"_s);
    REQUIRE(QTest::qWaitFor([&rig] { return !rig.controller->mission()->approvals().isEmpty(); }, kWaitMs));
    CHECK(rig.controller->submit(u"yes, do it"_s) == 0);
    REQUIRE(QTest::qWaitFor([&rig, id] { return rig.controller->mission()->task(id)->state == TaskState::Done; }, kWaitMs));
}

TEST_CASE("A board task asked for by voice says when it is done; the chat's own reply does not") {
    ControllerRig rig;
    rig.scripts = {{}, {Reply{u"Chat answer."_s, {}, {}}}, {Reply{u"Board answer."_s, {}, {}}}};
    rig.controller->start();
    QSignalSpy announced(rig.controller.get(), &AppController::announced);
    const quint64 chat = rig.controller->submit(u"what is the weather"_s, u"voice"_s);
    REQUIRE(QTest::qWaitFor([&rig, chat] { return rig.controller->mission()->task(chat)->state == TaskState::Done; }, kWaitMs));
    CHECK(announced.isEmpty());

    const QList<quint64> board = rig.controller->addTasks({u"summarise my notes"_s}, std::nullopt, u"voice"_s);
    REQUIRE(board.size() == 1);
    REQUIRE(QTest::qWaitFor([&rig, &board] { return rig.controller->mission()->task(board.first())->state == TaskState::Done; },
                            kWaitMs));
    REQUIRE(announced.size() == 1);
    CHECK(announced.at(0).at(0).toString().contains(u"is done"_s));
}
