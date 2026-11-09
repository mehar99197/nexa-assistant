// core/mission/MissionControl against real tools, policy, executor and audit, with scripted models handed out by a
// fake environment (one instance per task, as the app does): slots and the queue, pause and resume, questions routed
// to the task that asked, delegation, conversations carried on, and the live activity.

#include "core/audit/AuditLog.h"
#include "core/mission/DelegateTool.h"
#include "core/mission/MissionControl.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;
using Reply = test::ScriptedProvider::Reply;

namespace {

constexpr int kWaitMs = 5'000;

ToolCall call(const QString &id, const QString &name, const QString &arguments) {
    return ToolCall{id, name, arguments};
}

struct FakeEnvironment : ITaskEnvironment {
    Result<std::shared_ptr<IAiProvider>> createProvider(const ModelChoice &choice) override {
        created.append(choice);
        if (failWith) return *failWith;
        // Kept alive here too, so a test can read what a released provider was asked.
        auto provider = std::make_shared<test::ScriptedProvider>();
        if (!scripts.isEmpty()) provider->script = scripts.takeFirst();
        providers.append(provider);
        return std::shared_ptr<IAiProvider>(provider);
    }
    QString modelFor(const ModelChoice &choice) override { return choice.model.isEmpty() ? u"auto-model"_s : choice.model; }
    QString describeChoice(const ModelChoice &choice) override { return choice.isAuto() ? u"Auto"_s : choice.provider; }
    QList<ProviderFacts> providerFacts() override { return facts; }
    QString systemPrompt(const QString &source, AgentRole role) override {
        return u"prompt for %1 from %2"_s.arg(roleKey(role), source);
    }
    int maxSteps() override { return 8; }

    QList<QList<Reply>> scripts;                    // one per provider made, in order
    QList<ModelChoice> created;
    QList<std::shared_ptr<test::ScriptedProvider>> providers;
    QList<ProviderFacts> facts;
    std::optional<Error> failWith;
};

struct Board {
    explicit Board(int slotCount = 3) : audit(dir.filePath(u"audit.jsonl"_s)), executor(&registry, &policy, &audit) {
        registry.add(FunctionTool::sync(ToolSpec{u"echo"_s, u"Echo"_s, {}}, [](const QJsonObject &arguments) -> Result<ToolResult> {
            return ToolResult{u"echoed "_s + arguments.value(u"msg"_s).toString(), std::nullopt};
        }));
        MissionConfig config;
        config.maxConcurrent = slotCount;
        mission = std::make_unique<MissionControl>(&executor, &registry, &env, config);
        executor.setConfirmer([this](const ConfirmRequest &request, std::function<void(bool)> answer) {
            mission->confirm(request, std::move(answer));
        });
    }

    TaskState state(TaskId id) const { return mission->task(id) ? mission->task(id)->state : TaskState::Stopped; }
    bool waitFor(TaskId id, TaskState wanted) {
        return QTest::qWaitFor([this, id, wanted] { return mission->task(id) && mission->task(id)->state == wanted; }, kWaitMs);
    }
    TaskId ask(const QString &text, std::optional<TaskId> continues = std::nullopt) {
        TaskRequest request;
        request.text = text;
        request.continues = continues;
        return mission->add(request);
    }
    int auditLines() const {
        QFile file(dir.filePath(u"audit.jsonl"_s));
        if (!file.open(QIODevice::ReadOnly)) return 0;
        return int(file.readAll().count('\n'));
    }

    QTemporaryDir dir;
    FakeEnvironment env;
    ToolRegistry registry;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
    std::unique_ptr<MissionControl> mission;
};

bool offers(const ChatRequest &request, const QString &tool) {
    return std::any_of(request.tools.cbegin(), request.tools.cend(), [&tool](const ToolSpec &spec) { return spec.name == tool; });
}

} // namespace

TEST_CASE("Two tasks run at once, each on a provider instance of its own") {
    Board board;
    board.env.scripts = {{}, {}};               // both hang until stopped
    const TaskId first = board.ask(u"first question"_s);
    const TaskId second = board.ask(u"second question"_s);
    CHECK(board.state(first) == TaskState::Running);
    CHECK(board.state(second) == TaskState::Running);
    CHECK(board.mission->running() == 2);
    CHECK(board.mission->isBusy());
    REQUIRE(board.env.providers.size() == 2);
    REQUIRE(QTest::qWaitFor([&board] { return board.env.providers.at(1)->requests.size() == 1; }, kWaitMs));
    CHECK(board.env.providers.at(0)->requests.first().messages.last().text == u"first question"_s);
    CHECK(board.env.providers.at(1)->requests.first().messages.last().text == u"second question"_s);
    CHECK(board.env.providers.at(0)->requests.first().messages.first().text == u"prompt for general from gui"_s);

    board.mission->stopAll();
    CHECK(board.state(first) == TaskState::Stopped);
    CHECK(board.state(second) == TaskState::Stopped);
    CHECK_FALSE(board.mission->isBusy());
}

TEST_CASE("With one slot the second task waits, and starts when the first ends") {
    Board board(1);
    board.env.scripts = {{}, {Reply{u"B answered"_s, {}, {}}}};
    const TaskId first = board.ask(u"first"_s);
    const TaskId second = board.ask(u"second"_s);
    CHECK(board.state(second) == TaskState::Queued);
    CHECK(board.mission->queue() == QList<TaskId>{second});
    CHECK(board.env.providers.size() == 1);     // the waiting one has no provider yet
    board.mission->stop(first);
    REQUIRE(board.waitFor(second, TaskState::Done));
    CHECK(board.mission->queue().isEmpty());
    CHECK(board.mission->transcript(second)->lastReply() == u"B answered"_s);
}

TEST_CASE("The fast path runs at once, even with every slot taken") {
    Board board(1);
    board.env.scripts = {{}};
    const TaskId slow = board.ask(u"think hard"_s);
    TaskRequest quick;
    quick.text = u"echo hi"_s;
    quick.fastPath = IntentMatch{.toolName = u"echo"_s, .arguments = QJsonObject{{u"msg"_s, u"hi"_s}}};
    const TaskId fast = board.mission->add(quick);
    REQUIRE(board.waitFor(fast, TaskState::Done));
    CHECK(board.mission->task(fast)->fastPath);
    CHECK(board.state(slow) == TaskState::Running);
    const QList<TranscriptEvent> events = board.mission->transcript(fast)->events();
    CHECK(std::any_of(events.cbegin(), events.cend(), [](const TranscriptEvent &e) {
        return e.kind == TranscriptEvent::Kind::Action && e.text == u"echo"_s && e.extra == u"echoed hi"_s;
    }));
    board.mission->stopAll();
}

TEST_CASE("Pausing stops the run and keeps the conversation; resuming goes on from there") {
    Board board;
    board.env.scripts = {{}};
    const TaskId id = board.ask(u"write the report"_s);
    REQUIRE(QTest::qWaitFor([&board] { return board.env.providers.at(0)->lastStream != nullptr; }, kWaitMs));
    test::ScriptedStream *stream = board.env.providers.at(0)->lastStream;
    board.mission->pause(id);
    CHECK(board.state(id) == TaskState::Paused);
    CHECK(stream->aborted);
    CHECK(board.mission->running() == 0);
    CHECK_FALSE(board.mission->isBusy());

    board.env.providers.at(0)->script = {Reply{u"Resumed fine"_s, {}, {}}};
    board.mission->resume(id);
    REQUIRE(board.waitFor(id, TaskState::Done));
    REQUIRE(board.env.providers.at(0)->requests.size() == 2);
    const QList<ChatMessage> messages = board.env.providers.at(0)->requests.at(1).messages;
    CHECK(messages.last().text == u"Continue from where you paused."_s);
    CHECK(std::any_of(messages.cbegin(), messages.cend(), [](const ChatMessage &m) { return m.text == u"write the report"_s; }));
    CHECK(board.env.created.size() == 1);       // the same provider all along
}

TEST_CASE("A question goes to the task that asked: approve runs the tool, deny tells the model") {
    Board board;
    board.policy.setMode(PolicyMode::ConfirmAll);
    board.env.scripts = {{Reply{{}, {call(u"c1"_s, u"echo"_s, u"{\"msg\":\"a\"}"_s)}, {}}, Reply{u"A done"_s, {}, {}}},
                         {Reply{{}, {call(u"c2"_s, u"echo"_s, u"{\"msg\":\"b\"}"_s)}, {}}, Reply{u"B done"_s, {}, {}}}};
    QSignalSpy requested(board.mission.get(), &MissionControl::approvalRequested);
    const TaskId a = board.ask(u"do a"_s);
    const TaskId b = board.ask(u"do b"_s);
    REQUIRE(QTest::qWaitFor([&requested] { return requested.size() == 2; }, kWaitMs));
    CHECK(board.state(a) == TaskState::AwaitingYou);
    CHECK(board.state(b) == TaskState::AwaitingYou);
    CHECK(board.mission->awaiting() == 2);
    const QList<Approval> asked = board.mission->approvals();
    REQUIRE(asked.size() == 2);
    const Approval forA = asked.at(0).task == a ? asked.at(0) : asked.at(1);
    const Approval forB = asked.at(0).task == b ? asked.at(0) : asked.at(1);
    CHECK(forA.tool == u"echo"_s);
    CHECK(forA.agentId == board.mission->task(a)->agentId);

    board.mission->approve(forA.actionId);
    REQUIRE(board.waitFor(a, TaskState::Done));
    CHECK(board.state(b) == TaskState::AwaitingYou);
    board.mission->deny(forB.actionId);
    REQUIRE(board.waitFor(b, TaskState::Done));
    const ChatMessage &declined = board.env.providers.at(1)->requests.at(1).messages.last();
    CHECK(declined.text.contains(u"declined"_s));
    CHECK(board.auditLines() == 1);             // only the approved one ran
    CHECK(board.mission->approvals().isEmpty());
}

TEST_CASE("A lead hands work to another role, gives up its slot meanwhile, and gets the answer back") {
    Board board(1);
    board.env.scripts = {
        {Reply{{}, {call(u"d1"_s, u"delegate_task"_s, u"{\"role\":\"code\",\"task\":\"write hello in python\"}"_s)}, {}},
         Reply{u"The coder says hello"_s, {}, {}}},
        {Reply{u"print('hello')"_s, {}, {}}}};
    const TaskId lead = board.ask(u"plan the release"_s);
    REQUIRE(board.waitFor(lead, TaskState::Done));
    const QList<Task> all = board.mission->tasks();
    REQUIRE(all.size() == 2);
    const Task child = all.at(1);
    CHECK(child.parent == lead);
    CHECK(child.depth == 1);
    CHECK(child.role == AgentRole::Code);
    CHECK(child.source == u"agent"_s);
    CHECK(child.state == TaskState::Done);
    CHECK(child.agentId.startsWith(u"CODE-"_s));

    test::ScriptedProvider *leadModel = board.env.providers.at(0).get();
    test::ScriptedProvider *childModel = board.env.providers.at(1).get();
    CHECK(offers(leadModel->requests.at(0), u"delegate_task"_s));
    CHECK_FALSE(offers(childModel->requests.at(0), u"delegate_task"_s));
    CHECK(childModel->requests.at(0).messages.first().text == u"prompt for code from agent"_s);
    REQUIRE(leadModel->requests.size() == 2);
    CHECK(leadModel->requests.at(1).messages.last().text.contains(u"print('hello')"_s));
}

TEST_CASE("A sub-task cannot hand its work on, and stopping the lead stops it") {
    Board board;
    board.env.scripts = {
        {Reply{{}, {call(u"d1"_s, u"delegate_task"_s, u"{\"role\":\"web\",\"task\":\"look it up\"}"_s)}, {}}},
        {Reply{{}, {call(u"d2"_s, u"delegate_task"_s, u"{\"role\":\"code\",\"task\":\"again\"}"_s)}, {}}}};
    const TaskId lead = board.ask(u"research"_s);
    REQUIRE(QTest::qWaitFor([&board] { return board.env.providers.size() == 2 && board.env.providers.at(1)->requests.size() == 2; },
                            kWaitMs));
    // The child's call was answered with the reason, and nothing was made for it.
    CHECK(board.env.providers.at(1)->requests.at(1).messages.last().text.contains(u"cannot hand its work on"_s));
    CHECK(board.mission->tasks().size() == 2);
    CHECK(board.state(lead) == TaskState::Delegating);

    const TaskId child = board.mission->tasks().at(1).id;
    board.mission->stop(lead);
    CHECK(board.state(lead) == TaskState::Stopped);
    REQUIRE(board.waitFor(child, TaskState::Stopped));
    CHECK_FALSE(board.mission->isBusy());
}

TEST_CASE("A request that carries on a conversation sees it, on the same provider") {
    Board board;
    board.env.scripts = {{Reply{u"Paris"_s, {}, {}}}};
    const TaskId first = board.ask(u"capital of France?"_s);
    REQUIRE(board.waitFor(first, TaskState::Done));
    board.env.providers.at(0)->script = {Reply{u"About two million"_s, {}, {}}};
    const TaskId second = board.ask(u"and its population?"_s, first);
    REQUIRE(board.waitFor(second, TaskState::Done));
    CHECK(board.env.created.size() == 1);
    const QList<ChatMessage> messages = board.env.providers.at(0)->requests.at(1).messages;
    CHECK(std::any_of(messages.cbegin(), messages.cend(), [](const ChatMessage &m) { return m.text == u"Paris"_s; }));
    CHECK(messages.last().text == u"and its population?"_s);
}

TEST_CASE("A follow-up in a finished task goes on in its own conversation") {
    Board board;
    board.env.scripts = {{Reply{u"one"_s, {}, {}}}};
    const TaskId id = board.ask(u"count"_s);
    REQUIRE(board.waitFor(id, TaskState::Done));
    board.env.providers.at(0)->script = {Reply{u"two"_s, {}, {}}};
    board.mission->followUp(id, u"and the next?"_s, u"gui"_s);
    REQUIRE(QTest::qWaitFor([&board, id] { return board.mission->transcript(id)->lastReply() == u"two"_s; }, kWaitMs));
    REQUIRE(board.waitFor(id, TaskState::Done));
    CHECK(board.env.providers.at(0)->requests.at(1).messages.size() == 4);
}

TEST_CASE("An explicit model, and Settings → Mission for a role, choose the provider") {
    Board board;
    board.env.facts = {ProviderFacts{u"opencode"_s, u"opencode"_s, u"opencode"_s, {}, false, {}, true},
                       ProviderFacts{u"agy"_s, u"agy"_s, u"agy"_s, {}, false, {}, false}};
    board.env.scripts = {{Reply{u"ok"_s, {}, {}}}, {Reply{u"ok"_s, {}, {}}}};
    TaskRequest chosen;
    chosen.text = u"anything"_s;
    chosen.model = ModelChoice{u"codex"_s, u"gpt-x"_s};
    const TaskId explicitTask = board.mission->add(chosen);
    REQUIRE(board.waitFor(explicitTask, TaskState::Done));
    CHECK(board.env.created.last() == ModelChoice{u"codex"_s, u"gpt-x"_s});
    CHECK(board.env.providers.at(0)->requests.first().model == u"gpt-x"_s);

    MissionConfig config;
    config.roleModels.insert(u"code"_s, ModelChoice{u"agy"_s, u"gemini-pro"_s});
    board.mission->setConfig(config);
    const TaskId coding = board.ask(u"refactor the parser code"_s);
    CHECK(board.mission->task(coding)->role == AgentRole::Code);
    REQUIRE(board.waitFor(coding, TaskState::Done));
    CHECK(board.env.created.last() == ModelChoice{u"agy"_s, u"gemini-pro"_s});
}

TEST_CASE("A provider that cannot be made fails the task with the reason") {
    Board board;
    board.env.failWith = Error{ErrorCode::NotFound, u"No AI provider is configured — open Settings."_s};
    QSignalSpy failed(board.mission.get(), &MissionControl::taskReplyFailed);
    const TaskId id = board.ask(u"hello"_s);
    CHECK(board.state(id) == TaskState::Failed);
    CHECK(board.mission->task(id)->lastError.contains(u"No AI provider"_s));
    CHECK(failed.size() == 1);
    CHECK_FALSE(board.mission->isBusy());
}

TEST_CASE("Busy while anything runs or waits; what was done today is counted; the activity has its lane") {
    Board board;
    board.env.scripts = {{Reply{{}, {call(u"c1"_s, u"echo"_s, u"{\"msg\":\"x\"}"_s)}, {}}, Reply{u"done"_s, {}, {}}}};
    QSignalSpy busy(board.mission.get(), &MissionControl::busyChanged);
    const TaskId id = board.ask(u"search github.com for qt examples"_s);
    CHECK(board.mission->task(id)->role == AgentRole::Web);
    REQUIRE(board.waitFor(id, TaskState::Done));
    REQUIRE(busy.size() == 2);
    CHECK(busy.at(0).at(0).toBool());
    CHECK_FALSE(busy.at(1).at(0).toBool());
    CHECK(board.mission->doneToday() == 1);
    const QList<ActivityEvent> web = board.mission->feed().events(Lane::Web);
    CHECK(std::any_of(web.cbegin(), web.cend(), [](const ActivityEvent &e) { return e.text.startsWith(u"echo"_s); }));
    CHECK(board.mission->task(id)->progress == 100);

    QSignalSpy removed(board.mission.get(), &MissionControl::taskRemoved);
    board.mission->remove(id);
    CHECK(board.mission->task(id) == nullptr);
    CHECK(removed.size() == 1);
}

TEST_CASE("delegate_task outside a task is refused") {
    Board board;
    std::optional<Result<ToolResult>> outcome;
    board.executor.execute(u"delegate_task"_s, QJsonObject{{u"role"_s, u"code"_s}, {u"task"_s, u"x"_s}}, u"gui"_s,
                           [&outcome](Result<ToolResult> r) { outcome = std::move(r); });
    REQUIRE(outcome.has_value());
    REQUIRE_FALSE(outcome->isOk());
    CHECK(outcome->error().code == ErrorCode::NotSupported);
}
