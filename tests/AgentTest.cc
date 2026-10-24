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

#include <QFile>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

constexpr int kWaitMs = 5'000;

class ScriptedStream : public ChatStream {
public:
    void play(const QString &text, const QList<ToolCall> &calls, std::optional<Error> error) {
        QTimer::singleShot(0, this, [this, text, calls, error] {
            emitText(text);
            if (error) {
                fail(*error);
            } else {
                complete(calls.isEmpty() ? FinishReason::Stop : FinishReason::ToolCalls, calls);
            }
        });
    }
    bool aborted{false};

protected:
    void abortTransport() override { aborted = true; }
};

// One scripted reply per chat() call; an empty script leaves the stream hanging (for cancel tests).
class ScriptedProvider : public IAiProvider {
public:
    struct Reply {
        QString text;
        QList<ToolCall> calls;
        std::optional<Error> error;
    };

    QString id() const override { return u"scripted"_s; }
    QString displayName() const override { return u"Scripted"_s; }
    Capabilities capabilities() const override { return {.nativeTools = nativeTools}; }
    void listModels(std::function<void(Result<QStringList>)> done) override { done(QStringList{u"m"_s}); }

    std::shared_ptr<ChatStream> chat(const ChatRequest &request) override {
        requests.append(request);
        auto stream = std::make_shared<ScriptedStream>();
        lastStream = stream.get();
        if (!script.isEmpty()) {
            const Reply reply = script.takeFirst();
            stream->play(reply.text, reply.calls, reply.error);
        } else if (repeat) {
            stream->play(repeat->text, repeat->calls, repeat->error);
        }
        return stream;
    }

    QList<Reply> script;
    std::optional<Reply> repeat;
    bool nativeTools{true};
    QList<ChatRequest> requests;
    ScriptedStream *lastStream{nullptr};
};

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
    CHECK((QFile::permissions(f.audit.path()) & (QFileDevice::ReadOther | QFileDevice::ReadGroup)) == 0);
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
    f.executor.setConfirmer([&](const QString &question, std::function<void(bool)> answer) {
        questions.append(question);
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
    f.executor.setConfirmer([&](const QString &, std::function<void(bool)> answer) { pending = std::move(answer); });

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

    // The person sees the words, never the call markup — the reply is shown once it is complete.
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
