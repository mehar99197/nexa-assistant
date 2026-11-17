// The agent loop against a scripted fake model and real ToolExecutor/ToolRegistry/AuditLog.

#include "core/agent/Agent.h"
#include "core/agent/Conversation.h"
#include "core/ai/IAiProvider.h"
#include "core/audit/AuditLog.h"
#include "core/policy/PolicyEngine.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolExecutor.h"
#include "core/tools/ToolRegistry.h"

#include "QtStringMakers.h"
#include "ScriptedProvider.h"

#include <QFile>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

using test::ScriptedProvider;
using test::ScriptedStream;

ToolCall call(const QString &id, const QString &name, const QString &arguments) {
    return ToolCall{id, name, arguments};
}

struct Fixture {
    Fixture() : audit(auditDir.filePath(u"audit.jsonl"_s)), executor(&registry, &policy, &audit), agent(&executor, &registry) {
        registry.add(FunctionTool::sync(ToolSpec{u"echo"_s, u"Echo"_s, {}},
                                        [](const QJsonObject &arguments) -> Result<ToolResult> {
            return ToolResult{u"echoed "_s + arguments.value(u"msg"_s).toString(), std::nullopt};
        }));
        registry.add(FunctionTool::sync(ToolSpec{u"look"_s, u"Screenshot"_s, {}, false},
                                        [](const QJsonObject &) -> Result<ToolResult> {
            return ToolResult{u"a screenshot"_s, ImageAttachment{"JPEG", u"image/jpeg"_s}};
        }));
        // Never finishes until cancelled.
        registry.add(std::make_unique<FunctionTool>(
            ToolSpec{u"slow"_s, u"Slow"_s, {}},
            [this](const QJsonObject &, ITool::Done done) { pendingSlow = std::move(done); },
            [this] {
                if (pendingSlow) std::exchange(pendingSlow, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
            }));
        conversation.addUser(u"hello"_s);
        QObject::connect(&agent, &Agent::finished, [this](FinishReason reason) { finish = reason; });
        QObject::connect(&agent, &Agent::failed, [this](const Error &e) { error = e; });
        QObject::connect(&agent, &Agent::toolFinished, [this](const QString &tool, bool ok, const QString &, qint64) {
            toolRuns.append(tool + (ok ? u":ok"_s : u":fail"_s));
        });
    }

    bool waitDone() {
        return QTest::qWaitFor([this] { return finish.has_value() || error.has_value(); }, kWaitMs);
    }

    QTemporaryDir auditDir;
    ScriptedProvider provider;
    ToolRegistry registry;
    PolicyEngine policy;
    AuditLog audit;
    ToolExecutor executor;
    Agent agent;
    Conversation conversation{u"system prompt"_s};
    ITool::Done pendingSlow;
    std::optional<FinishReason> finish;
    std::optional<Error> error;
    QStringList toolRuns;
};

} // namespace

TEST_CASE("Agent returns a plain answer and offers the tools") {
    Fixture f;
    f.provider.script = {{u"Hi there"_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.finish == FinishReason::Stop);
    CHECK(f.conversation.messages().last().text == u"Hi there"_s);
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests[0].tools.size() == 3);
    CHECK_FALSE(f.agent.isRunning());
}

TEST_CASE("Agent runs tool calls, feeds the results back and audits them") {
    Fixture f;
    f.provider.script = {{u"Let me check."_s, {call(u"c1"_s, u"echo"_s, uR"({"msg":"x"})"_s)}, {}}, {u"Done: x"_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.finish == FinishReason::Stop);
    CHECK(f.toolRuns == QStringList{u"echo:ok"_s});

    const QList<ChatMessage> &messages = f.conversation.messages();
    REQUIRE(messages.size() == 4);
    CHECK(messages[1].role == Role::Assistant);
    CHECK(messages[1].toolCalls.size() == 1);
    CHECK(messages[2].role == Role::Tool);
    CHECK(messages[2].toolCallId == u"c1"_s);
    CHECK(messages[2].text == u"echoed x"_s);
    CHECK(messages[3].text == u"Done: x"_s);

    REQUIRE(f.provider.requests.size() == 2);
    CHECK(f.provider.requests[1].messages.last().role == Role::Tool);

    QFile audit(f.audit.path());
    REQUIRE(audit.open(QIODevice::ReadOnly));
    const QJsonObject entry = QJsonDocument::fromJson(audit.readLine()).object();
    CHECK(entry.value(u"tool"_s).toString() == u"echo"_s);
    CHECK(entry.value(u"source"_s).toString() == u"gui"_s);
    CHECK(entry.value(u"ok"_s).toBool());
#ifdef Q_OS_UNIX
    CHECK((QFile::permissions(f.audit.path()) & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0);
#endif
}

TEST_CASE("Agent reports bad arguments and unknown tools to the model and continues") {
    Fixture f;
    f.provider.script = {{{}, {call(u"c1"_s, u"echo"_s, u"{not json"_s), call(u"c2"_s, u"nope"_s, u"{}"_s)}, {}},
                         {u"Sorry."_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.toolRuns == QStringList{u"echo:fail"_s, u"nope:fail"_s});
    const QList<ChatMessage> &messages = f.conversation.messages();
    CHECK(messages[2].text.contains(u"not a valid JSON object"_s));
    CHECK(messages[3].text.contains(u"Unknown tool"_s));
}

TEST_CASE("Agent sends tool images back as a user message") {
    Fixture f;
    f.provider.script = {{{}, {call(u"c1"_s, u"look"_s, u"{}"_s)}, {}}, {u"I see a desktop."_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 2);
    const ChatMessage &last = f.provider.requests[1].messages.last();
    CHECK(last.role == Role::User);
    REQUIRE(last.images.size() == 1);
    CHECK(last.images[0].data == "JPEG");
}

TEST_CASE("Agent keeps a withheld tool from the model and refuses a call to it with the reason") {
    Fixture f;
    f.registry.withhold(u"look"_s, u"the user is not sharing the screen"_s);
    f.provider.script = {{{}, {call(u"c1"_s, u"look"_s, u"{}"_s)}, {}}, {u"Then I cannot see it."_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.finish == FinishReason::Stop);
    // Not offered: two tools instead of three, in every request.
    REQUIRE(f.provider.requests.size() == 2);
    for (const ChatRequest &request : f.provider.requests) {
        CHECK(request.tools.size() == 2);
        for (const ToolSpec &spec : request.tools) CHECK(spec.name != u"look"_s);
    }
    // Called anyway: the model is told why, and nothing ran (no image came back).
    CHECK(f.toolRuns == QStringList{u"look:fail"_s});
    const ChatMessage &result = f.provider.requests[1].messages.last();
    CHECK(result.role == Role::Tool);
    CHECK(result.text.contains(u"not sharing the screen"_s));
    CHECK(result.images.isEmpty());
    const QFile audit(f.audit.path());
    CHECK((!audit.exists() || audit.size() == 0));

    // Offered again, it runs as before.
    f.registry.offer(u"look"_s);
    CHECK(f.registry.specs().size() == 3);
    CHECK(f.registry.find(u"look"_s) != nullptr);
}

TEST_CASE("Agent leaves images out for a model that refuses them, and says so") {
    Fixture f;
    f.conversation.addUser(u"what is this?"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    f.provider.script = {{{}, {}, Error{ErrorCode::Http, u"HTTP 400: messages[1].content[1].type has unsupported value \"image_url\""_s}},
                         {u"I cannot see images with this model."_s, {}, {}}};
    QStringList notices;
    QObject::connect(&f.agent, &Agent::notice, [&notices](const QString &notice) { notices.append(notice); });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.finish == FinishReason::Stop);
    CHECK_FALSE(f.error.has_value());
    // The same step went out twice: with the image, then without it and with a note instead.
    REQUIRE(f.provider.requests.size() == 2);
    CHECK_FALSE(f.provider.requests[0].messages.last().images.isEmpty());
    const ChatMessage &retried = f.provider.requests[1].messages.last();
    CHECK(retried.images.isEmpty());
    CHECK(retried.text.startsWith(u"what is this?"_s));
    CHECK(retried.text.contains(u"cannot see images"_s));
    REQUIRE(notices.size() == 1);
    CHECK(notices[0].contains(u"m cannot see images"_s));
    CHECK(f.agent.cannotSeeImages(u"m"_s));
    CHECK_FALSE(f.agent.cannotSeeImages(u"other"_s));
    CHECK(f.conversation.messages().last().text == u"I cannot see images with this model."_s);

    // From then on that model's requests go without images from the start, and quietly.
    f.conversation.addUser(u"and this one?"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    f.provider.script = {{u"Still cannot."_s, {}, {}}};
    f.finish.reset();
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 3);
    CHECK(f.provider.requests[2].messages.last().images.isEmpty());
    CHECK(notices.size() == 1);
}

TEST_CASE("Agent reports an error unrelated to images as before, images or not") {
    Fixture f;
    f.conversation.addUser(u"look"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    f.provider.script = {{{}, {}, Error{ErrorCode::Http, u"HTTP 500: the upstream is down"_s}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.error.has_value());
    CHECK(f.error->message.contains(u"upstream"_s));
    CHECK(f.provider.requests.size() == 1);
    CHECK_FALSE(f.agent.cannotSeeImages(u"m"_s));

    // A rate limit that names a vision model, or an outage that mentions images, is no refusal.
    Fixture g;
    g.conversation.addUser(u"look"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    g.provider.script = {{{}, {}, Error{ErrorCode::Http, u"HTTP 429: llama-vision is rate limited"_s}}};
    g.agent.run(&g.provider, &g.conversation, u"llama-vision"_s, u"gui"_s);
    REQUIRE(g.waitDone());
    CHECK(g.error.has_value());
    CHECK(g.provider.requests.size() == 1);
    CHECK_FALSE(g.agent.cannotSeeImages(u"llama-vision"_s));
    Fixture h;
    h.conversation.addUser(u"look"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    h.provider.script = {{{}, {}, Error{ErrorCode::Http, u"HTTP 400: the image exceeds the 5 MB maximum"_s}}};
    h.agent.run(&h.provider, &h.conversation, u"m"_s, u"gui"_s);
    REQUIRE(h.waitDone());
    CHECK(h.provider.requests.size() == 1);     // a refusal of this image, not of images
    CHECK_FALSE(h.agent.cannotSeeImages(u"m"_s));
}

TEST_CASE("Agent does not remember a model as blind when the retry without images fails too") {
    Fixture f;
    f.conversation.addUser(u"look"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    f.provider.script = {{{}, {}, Error{ErrorCode::Http, u"[400]: content[1].type has unsupported value \"image_url\""_s}},
                         {{}, {}, Error{ErrorCode::Http, u"HTTP 503: capacity is temporarily unavailable"_s}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.error.has_value());
    CHECK(f.error->message.contains(u"capacity"_s));
    REQUIRE(f.provider.requests.size() == 2);
    CHECK(f.provider.requests[1].messages.last().images.isEmpty());
    CHECK_FALSE(f.agent.cannotSeeImages(u"m"_s));

    // The next run sends the image again — nothing was learnt.
    f.provider.script = {{u"fine"_s, {}, {}}};
    f.error.reset();
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 3);
    CHECK_FALSE(f.provider.requests[2].messages.last().images.isEmpty());
}

TEST_CASE("Agent::cancel during a tool closes the call and stops") {
    Fixture f;
    f.provider.script = {{{}, {call(u"c1"_s, u"slow"_s, u"{}"_s), call(u"c2"_s, u"echo"_s, u"{}"_s)}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(QTest::qWaitFor([&f] { return bool(f.pendingSlow); }, kWaitMs));
    f.agent.cancel();
    CHECK(f.finish == FinishReason::Cancelled);
    QTest::qWait(30);
    CHECK(f.provider.requests.size() == 1);  // no further round after cancel
    const QList<ChatMessage> &messages = f.conversation.messages();
    REQUIRE(messages.size() == 4);
    CHECK(messages[2].text == u"Cancelled by the user."_s);
    CHECK(messages[3].toolCallId == u"c2"_s);
}

TEST_CASE("Agent::cancel during streaming aborts the stream") {
    Fixture f;  // empty script: the stream never finishes
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.provider.lastStream != nullptr);
    f.agent.cancel();
    CHECK(f.finish == FinishReason::Cancelled);
    CHECK_FALSE(f.agent.isRunning());
}

TEST_CASE("Agent stops after the step limit") {
    Fixture f;
    f.provider.repeat = ScriptedProvider::Reply{{}, {call(u"c"_s, u"echo"_s, u"{}"_s)}, {}};
    f.agent.setMaxSteps(3);
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.error.has_value());
    CHECK(f.error->message.contains(u"3 tool rounds"_s));
    CHECK(f.provider.requests.size() == 3);
}

TEST_CASE("Agent reports stream failures") {
    Fixture f;
    f.provider.script = {{u"partial"_s, {}, Error{ErrorCode::Http, u"HTTP 500"_s}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.error.has_value());
    CHECK(f.error->message == u"HTTP 500"_s);
    CHECK(f.conversation.messages().last().text == u"partial"_s);
}

TEST_CASE("ToolExecutor asks for confirmation and honours the answer") {
    Fixture f;
    f.policy.setMode(PolicyMode::ConfirmAll);
    QStringList questions;
    bool allow = false;
    f.executor.setConfirmer([&](const ConfirmRequest &request, std::function<void(bool)> answer) {
        questions.append(request.question);
        answer(allow);
    });

    std::optional<Result<ToolResult>> declined;
    f.executor.execute(u"echo"_s, {{u"msg"_s, u"a"_s}}, u"gui"_s, [&](Result<ToolResult> r) { declined = std::move(r); });
    REQUIRE(declined.has_value());
    CHECK(declined->error().code == ErrorCode::PermissionDenied);

    allow = true;
    std::optional<Result<ToolResult>> allowed;
    f.executor.execute(u"echo"_s, {{u"msg"_s, u"b"_s}}, u"gui"_s, [&](Result<ToolResult> r) { allowed = std::move(r); });
    REQUIRE(allowed.has_value());
    CHECK(allowed->value().text == u"echoed b"_s);
    CHECK(questions.size() == 2);

    std::optional<Result<ToolResult>> readOnly;  // non-mutating tools never ask
    f.executor.execute(u"look"_s, {}, u"gui"_s, [&](Result<ToolResult> r) { readOnly = std::move(r); });
    CHECK(readOnly->isOk());
    CHECK(questions.size() == 2);
}

TEST_CASE("A tool that goes while its question is open is not run when the answer comes") {
    Fixture f;
    f.policy.setMode(PolicyMode::ConfirmAll);
    std::function<void(bool)> pending;
    f.executor.setConfirmer([&](const ConfirmRequest &, std::function<void(bool)> answer) { pending = std::move(answer); });

    std::optional<Result<ToolResult>> result;
    f.executor.execute(u"echo"_s, {{u"msg"_s, u"a"_s}}, u"gui"_s, [&](Result<ToolResult> r) { result = std::move(r); });
    REQUIRE(pending);
    CHECK_FALSE(result.has_value());
    f.registry.remove(u"echo"_s);   // e.g. its connector listed its tools again: the old one is deleted
    pending(true);
    REQUIRE(result.has_value());
    REQUIRE_FALSE(result->isOk());
    CHECK(result->error().message.contains(u"no longer available"_s));
}

TEST_CASE("A model that cannot call tools is taught the convention, and its calls are read from the text") {
    Fixture f;
    f.provider.nativeTools = false;
    f.provider.script = {
        {u"Let me echo that.\n<tool_call>{\"name\": \"echo\", \"arguments\": {\"msg\": \"x\"}}</tool_call>"_s, {}, {}},
        {u"It said: echoed x"_s, {}, {}},
    };
    QStringList shown;
    QObject::connect(&f.agent, &Agent::textDelta, [&shown](const QString &delta) { shown.append(delta); });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.finish == FinishReason::Stop);
    CHECK(f.toolRuns == QStringList{u"echo:ok"_s});

    // No native tools: they are described in the system prompt instead.
    REQUIRE(f.provider.requests.size() == 2);
    const ChatRequest &first = f.provider.requests[0];
    CHECK(first.tools.isEmpty());
    REQUIRE(first.messages.first().role == Role::System);
    CHECK(first.messages.first().text.startsWith(u"system prompt"_s));
    CHECK(first.messages.first().text.contains(u"<tool_call>"_s));
    CHECK(first.messages.first().text.contains(u"- echo: Echo"_s));

    // The second request carries the call and its result as text, with no "tool" role in sight.
    const ChatRequest &second = f.provider.requests[1];
    bool resultAsText = false;
    for (const ChatMessage &message : second.messages) {
        CHECK(message.role != Role::Tool);
        CHECK(message.toolCalls.isEmpty());
        if (message.role == Role::User && message.text.contains(u"<tool_result name=\"echo\">"_s)) {
            resultAsText = message.text.contains(u"echoed x"_s);
        }
    }
    CHECK(resultAsText);

    // The person sees the words, never the call markup.
    CHECK(shown == QStringList{u"Let me echo that."_s, u"It said: echoed x"_s});
}

TEST_CASE("A call a native model writes as text is still made") {
    Fixture f;
    f.provider.script = {
        {u"<tool_call>{\"name\": \"echo\", \"arguments\": {\"msg\": \"y\"}}</tool_call>"_s, {}, {}},
        {u"ok"_s, {}, {}},
    };
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.toolRuns == QStringList{u"echo:ok"_s});
    const QList<ChatMessage> &messages = f.conversation.messages();
    // hello, the call (no leftover markup), its result, the answer
    REQUIRE(messages.size() == 4);
    CHECK(messages[1].text.isEmpty());
    REQUIRE(messages[1].toolCalls.size() == 1);
    CHECK(messages[1].toolCalls[0].id == u"text_1_1"_s);
    CHECK(messages[2].text == u"echoed y"_s);
}

TEST_CASE("A text-tools reply streams its prose and never shows the call markup") {
    Fixture f;
    f.provider.nativeTools = false;
    f.provider.script = {
        {{}, {}, {}, {u"Opening the calculator.\n<tool"_s,
                      u"_call>{\"name\": \"echo\", \"arguments\": {\"msg\": \"x\"}}</tool_call>"_s}},
        {{}, {}, {}, {u"Do"_s, u"ne."_s}},
    };
    QStringList shown;
    int deltasBeforeTool = -1;
    QObject::connect(&f.agent, &Agent::textDelta, [&shown](const QString &delta) { shown.append(delta); });
    QObject::connect(&f.agent, &Agent::toolStarted, [&](const QString &, const QJsonObject &) {
        if (deltasBeforeTool < 0) deltasBeforeTool = int(shown.size());
    });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.toolRuns == QStringList{u"echo:ok"_s});
    const QString all = shown.join(QString());
    CHECK(all.contains(u"Opening the calculator."_s));
    CHECK(all.contains(u"Done."_s));
    CHECK_FALSE(all.contains(u"<tool"_s));
    CHECK(deltasBeforeTool == 1);       // the prose was on screen before the tool ran
    CHECK(shown.size() >= 3);           // "Done." arrived in pieces, as streamed
}

TEST_CASE("A reply that is JSON but not a call is shown in full at the end") {
    Fixture f;
    f.provider.nativeTools = false;
    f.provider.script = {{{}, {}, {}, {u"{\"answer\": "_s, u"42}"_s}}};
    QStringList shown;
    QObject::connect(&f.agent, &Agent::textDelta, [&shown](const QString &delta) { shown.append(delta); });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(shown.join(QString()) == u"{\"answer\": 42}"_s);
}

TEST_CASE("A text-tools reply that starts with spaces is not shown twice") {
    Fixture f;
    f.provider.nativeTools = false;
    f.provider.script = {{{}, {}, {}, {u"\n  Hel"_s, u"lo there"_s}}};
    QStringList shown;
    QObject::connect(&f.agent, &Agent::textDelta, [&shown](const QString &delta) { shown.append(delta); });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(shown.join(QString()) == u"Hello there"_s);
}

TEST_CASE("A fallback that answered is reported") {
    Fixture f;
    f.provider.script = {{u"hi"_s, {}, {}, {}, u"Two · m2"_s}};
    QStringList reroutes;
    QObject::connect(&f.agent, &Agent::rerouted, [&](const QString &route, const QString &reason) {
        reroutes.append(route + u" | "_s + reason);
    });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(reroutes == QStringList{u"Two · m2 | One: down"_s});
}

TEST_CASE("A provider that cannot see gets the words and a note, and the user is told once") {
    Fixture f;
    f.provider.vision = false;
    f.provider.script = {{u"I cannot look at it."_s, {}, {}}, {u"Still cannot."_s, {}, {}}};
    QStringList notices;
    QObject::connect(&f.agent, &Agent::notice, [&notices](const QString &message) { notices.append(message); });
    f.conversation.addUser(u"what is on this screenshot?"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 1);
    for (const ChatMessage &message : f.provider.requests.first().messages) CHECK(message.images.isEmpty());
    CHECK(f.provider.requests.first().messages.last().text.contains(u"cannot see images"_s));
    CHECK(notices.size() == 1);

    f.finish.reset();
    f.conversation.addUser(u"and now?"_s);
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(notices.size() == 1);     // said once, not at every request that still carries the picture
}

TEST_CASE("A native first choice falling back to a text-only provider never shows the call markup") {
    Fixture f;
    f.provider.nativeTools = true;     // the first choice could call tools; the fallback that answered cannot
    ScriptedProvider::Reply first;
    first.route = u"agy · m2"_s;
    first.chunks = {u"Checking.\n<tool_call>{\"name\": \"echo\", \"arguments\": {\"msg\": \"x\"}}</tool_call>"_s};
    f.provider.script = {first, {u"Done."_s, {}, {}}};
    QStringList shown;
    QObject::connect(&f.agent, &Agent::textDelta, [&shown](const QString &delta) { shown.append(delta); });
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    CHECK(f.toolRuns == QStringList{u"echo:ok"_s});
    const QString all = shown.join(QString());
    CHECK(all.contains(u"Checking."_s));
    CHECK_FALSE(all.contains(u"<tool"_s));
}

TEST_CASE("A picture is kept when some provider of the chain can see it, though the first choice cannot") {
    Fixture f;
    f.provider.vision = false;      // the first choice (agy) cannot…
    f.provider.images = true;       // …but the chain can take the picture to one that can
    f.provider.script = {{u"A red square."_s, {}, {}}};
    QStringList notices;
    QObject::connect(&f.agent, &Agent::notice, [&notices](const QString &message) { notices.append(message); });
    f.conversation.addUser(u"what is on this picture?"_s, {ImageAttachment{"PNG", u"image/png"_s}});
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 1);
    CHECK(f.provider.requests.first().messages.last().images.size() == 1);
    CHECK(notices.isEmpty());
}

TEST_CASE("An agent given a task runs its tools for that task, and says when each step starts") {
    Fixture f;
    f.agent.setTask(5);
    QSignalSpy started(&f.executor, &ToolExecutor::actionStarted);
    QSignalSpy steps(&f.agent, &Agent::stepStarted);
    f.provider.script = {{{}, {call(u"c1"_s, u"echo"_s, u"{\"msg\":\"x\"}"_s)}, {}}, {u"Done."_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(started.size() == 1);
    CHECK(started.at(0).at(4).toULongLong() == 5);
    REQUIRE(steps.size() == 2);
    CHECK(steps.at(0).at(0).toInt() == 1);
    CHECK(steps.at(1).at(0).toInt() == 2);
}

TEST_CASE("A hidden tool is not offered, and a call to it is answered with the reason, nothing run") {
    Fixture f;
    f.agent.setHiddenTools({u"echo"_s}, u"it is not available in a sub-task"_s);
    f.provider.script = {{{}, {call(u"c1"_s, u"echo"_s, u"{}"_s)}, {}}, {u"Fine."_s, {}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(f.waitDone());
    REQUIRE(f.provider.requests.size() == 2);
    for (const ToolSpec &spec : f.provider.requests[0].tools) CHECK(spec.name != u"echo"_s);
    CHECK(f.toolRuns == QStringList{u"echo:fail"_s});
    CHECK(f.provider.requests[1].messages.last().text.contains(u"not available in a sub-task"_s));
    const QFile audit(f.audit.path());
    CHECK((!audit.exists() || audit.size() == 0));
}

TEST_CASE("Cancelling with a note closes the open call with it") {
    Fixture f;
    f.provider.script = {{{}, {call(u"c1"_s, u"slow"_s, u"{}"_s)}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(QTest::qWaitFor([&f] { return bool(f.pendingSlow); }, kWaitMs));
    f.agent.cancel(u"Paused by the user."_s);
    CHECK(f.finish == FinishReason::Cancelled);
    CHECK(f.conversation.messages().last().text == u"Paused by the user."_s);
}

TEST_CASE("Cancelling an agent with a task stops only that task's tool") {
    Fixture f;
    f.agent.setTask(5);
    std::optional<Result<ToolResult>> other;
    ITool::Done otherSlow;
    f.registry.add(std::make_unique<FunctionTool>(
        ToolSpec{u"slow_other"_s, u"Slow"_s, {}}, [&otherSlow](const QJsonObject &, ITool::Done done) { otherSlow = std::move(done); },
        [&otherSlow] {
            if (otherSlow) std::exchange(otherSlow, nullptr)(Error{ErrorCode::Cancelled, u"Cancelled"_s});
        }));
    f.executor.execute(u"slow_other"_s, {}, ToolContext{u"gui"_s, 6}, [&other](Result<ToolResult> r) { other = std::move(r); });
    f.provider.script = {{{}, {call(u"c1"_s, u"slow"_s, u"{}"_s)}, {}}};
    f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
    REQUIRE(QTest::qWaitFor([&f] { return bool(f.pendingSlow); }, kWaitMs));
    f.agent.cancel();
    CHECK_FALSE(f.pendingSlow);     // its own tool was cancelled
    CHECK_FALSE(other.has_value()); // the other task's was not
    REQUIRE(otherSlow);
    std::exchange(otherSlow, nullptr)(ToolResult{u"fine"_s, {}});
    REQUIRE(other.has_value());
    CHECK(other->isOk());
}

TEST_CASE("A provider that cannot see is told the screen can still be read as text, when that tool is offered") {
    Fixture f;
    f.provider.vision = false;
    f.provider.script = {{u"I cannot look at it."_s, {}, {}}};
    QStringList notices;
    QObject::connect(&f.agent, &Agent::notice, [&notices](const QString &message) { notices.append(message); });
    SECTION("without screen_snapshot the notice is as before") {
        f.conversation.addUser(u"what is on this screenshot?"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
        f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
        REQUIRE(f.waitDone());
        REQUIRE(notices.size() == 1);
        CHECK(notices.first().endsWith(u"needed to look at the screen."_s));
    }
    SECTION("with it, the way out is named") {
        f.registry.add(FunctionTool::sync(ToolSpec{u"screen_snapshot"_s, u"The screen as text"_s, {}, false},
                                          [](const QJsonObject &) -> Result<ToolResult> { return ToolResult{u"outline"_s, std::nullopt}; }));
        f.conversation.addUser(u"what is on this screenshot?"_s, {ImageAttachment{"JPEG", u"image/jpeg"_s}});
        f.agent.run(&f.provider, &f.conversation, u"m"_s, u"gui"_s);
        REQUIRE(f.waitDone());
        REQUIRE(notices.size() == 1);
        CHECK(notices.first().endsWith(u"The screen can still be read as text with screen_snapshot."_s));
    }
}
