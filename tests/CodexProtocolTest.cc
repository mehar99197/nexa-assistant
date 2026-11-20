// core/ai/CodexProtocol and CodexEvents: the wire of `codex app-server`, pure.

#include "core/ai/CodexEvents.h"
#include "core/ai/CodexProtocol.h"
#include "core/ai/ProviderChain.h"

#include "QtStringMakers.h"

#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QJsonObject object(const char *json) {
    return QJsonDocument::fromJson(QByteArray(json)).object();
}

CodexMessage message(const char *json) {
    return CodexProtocol::decode(QByteArray(json));
}

} // namespace

TEST_CASE("Codex requests are one line of JSON without a jsonrpc field", "[codex]") {
    const QByteArray line = CodexProtocol::request(7, u"account/read"_s, QJsonObject{{u"x"_s, 1}});
    REQUIRE(line.endsWith('\n'));
    CHECK(line.count('\n') == 1);
    const QJsonObject parsed = QJsonDocument::fromJson(line).object();
    CHECK(parsed.value(u"id"_s).toInt() == 7);
    CHECK(parsed.value(u"method"_s).toString() == u"account/read"_s);
    CHECK(parsed.value(u"params"_s).toObject().value(u"x"_s).toInt() == 1);
    CHECK_FALSE(parsed.contains(u"jsonrpc"_s));

    const QJsonObject note = QJsonDocument::fromJson(CodexProtocol::notification(u"initialized"_s)).object();
    CHECK(note.value(u"method"_s).toString() == u"initialized"_s);
    CHECK_FALSE(note.contains(u"id"_s));
}

TEST_CASE("Codex answers to the server's requests carry the server's own id", "[codex]") {
    const QJsonObject refused =
        QJsonDocument::fromJson(CodexProtocol::errorResponse(QJsonValue(u"srv-1"_s), -32601, u"no"_s)).object();
    CHECK(refused.value(u"id"_s).toString() == u"srv-1"_s);
    CHECK(refused.value(u"error"_s).toObject().value(u"code"_s).toInt() == -32601);
    const QJsonObject ok = QJsonDocument::fromJson(CodexProtocol::response(QJsonValue(4), {})).object();
    CHECK(ok.value(u"id"_s).toInt() == 4);
    CHECK(ok.contains(u"result"_s));
}

TEST_CASE("Codex lines are told apart by what they carry", "[codex]") {
    const CodexMessage reply = message(R"({"id":3,"result":{"a":1}})");
    CHECK(reply.kind == CodexMessage::Kind::Response);
    CHECK(reply.id.toInt() == 3);
    CHECK(reply.result.value(u"a"_s).toInt() == 1);

    const CodexMessage failure = message(R"({"id":4,"error":{"code":-32600,"message":"bad thing"}})");
    CHECK(failure.kind == CodexMessage::Kind::Failure);
    CHECK(failure.errorCode == -32600);
    CHECK(failure.errorMessage == u"bad thing"_s);

    const CodexMessage note = message(R"({"method":"turn/started","params":{"threadId":"t"}})");
    CHECK(note.kind == CodexMessage::Kind::Notification);
    CHECK(note.method == u"turn/started"_s);
    CHECK(note.params.value(u"threadId"_s).toString() == u"t"_s);

    const CodexMessage request = message(R"({"id":"s9","method":"item/tool/call","params":{}})");
    CHECK(request.kind == CodexMessage::Kind::ServerRequest);
    CHECK(request.id.toString() == u"s9"_s);

    CHECK(message("Loading the model, please wait").kind == CodexMessage::Kind::Invalid);
    CHECK(message("").kind == CodexMessage::Kind::Invalid);
    CHECK(message("[1,2]").kind == CodexMessage::Kind::Invalid);
    CHECK(message(R"({"unrelated":true})").kind == CodexMessage::Kind::Invalid);
}

TEST_CASE("The server starts with every tool feature off and web search disabled", "[codex]") {
    const QStringList arguments = CodexProtocol::serverArguments();
    CHECK(arguments.first() == u"app-server"_s);
    for (const QString &feature : {u"shell_tool"_s, u"unified_exec"_s, u"view_image"_s, u"sleep_tool"_s,
                                   u"image_generation"_s, u"apps"_s, u"plugins"_s, u"multi_agent"_s, u"browser_use"_s,
                                   u"computer_use"_s, u"in_app_browser"_s, u"hooks"_s, u"goals"_s, u"memories"_s,
                                   u"skill_search"_s}) {
        const qsizetype at = arguments.indexOf(feature);
        INFO("feature " << feature.toStdString());
        REQUIRE(at > 0);
        CHECK(arguments.at(at - 1) == u"--disable"_s);
    }
    const qsizetype config = arguments.indexOf(u"-c"_s);
    REQUIRE(config > 0);
    CHECK(arguments.contains(u"web_search=\"disabled\""_s));
}

TEST_CASE("initialize names the client nexa and asks for nothing experimental", "[codex]") {
    const QJsonObject params = CodexProtocol::initializeParams(u"1.2.3"_s);
    CHECK(params.value(u"clientInfo"_s).toObject().value(u"name"_s).toString() == u"nexa"_s);
    CHECK(params.value(u"clientInfo"_s).toObject().value(u"version"_s).toString() == u"1.2.3"_s);
#ifdef Q_OS_WIN
    CHECK(params.value(u"capabilities"_s).toObject().value(u"experimentalApi"_s).toBool(false));
#else
    CHECK_FALSE(params.value(u"capabilities"_s).toObject().value(u"experimentalApi"_s).toBool(true));
#endif
    CHECK_FALSE(params.value(u"capabilities"_s).toObject().value(u"requestAttestation"_s).toBool(true));
}

TEST_CASE("A thread is ephemeral, read-only, never asks, and switches the user's MCP servers off", "[codex]") {
    const QJsonObject params = CodexProtocol::threadStartParams(u"gpt-x"_s, u"/work"_s, u"Be brief."_s,
                                                                {u"rea"_s, u"github"_s});
    CHECK(params.value(u"model"_s).toString() == u"gpt-x"_s);
    CHECK(params.value(u"cwd"_s).toString() == u"/work"_s);
    CHECK(params.value(u"approvalPolicy"_s).toString() == u"never"_s);
    CHECK(params.value(u"sandbox"_s).toString() == u"read-only"_s);
    CHECK(params.value(u"ephemeral"_s).toBool());
    CHECK(params.value(u"developerInstructions"_s).toString() == u"Be brief."_s);
    const QJsonObject servers = params.value(u"config"_s).toObject().value(u"mcp_servers"_s).toObject();
    CHECK(servers.keys().size() == 2);
    CHECK_FALSE(servers.value(u"rea"_s).toObject().value(u"enabled"_s).toBool(true));
    CHECK_FALSE(servers.value(u"github"_s).toObject().value(u"enabled"_s).toBool(true));
}

TEST_CASE("A thread without a model leaves it to Codex, and without servers sets none", "[codex]") {
    const QJsonObject params = CodexProtocol::threadStartParams({}, u"/work"_s, {}, {});
    CHECK_FALSE(params.contains(u"model"_s));
    CHECK_FALSE(params.contains(u"developerInstructions"_s));
    CHECK_FALSE(params.value(u"config"_s).toObject().contains(u"mcp_servers"_s));
}

TEST_CASE("A turn carries one text input and the model", "[codex]") {
    const QJsonObject params = CodexProtocol::turnStartParams(u"th-1"_s, u"hello"_s, u"gpt-x"_s);
    CHECK(params.value(u"threadId"_s).toString() == u"th-1"_s);
    CHECK(params.value(u"model"_s).toString() == u"gpt-x"_s);
    const QJsonArray input = params.value(u"input"_s).toArray();
    REQUIRE(input.size() == 1);
    CHECK(input.first().toObject().value(u"type"_s).toString() == u"text"_s);
    CHECK(input.first().toObject().value(u"text"_s).toString() == u"hello"_s);
    CHECK(input.first().toObject().contains(u"text_elements"_s));

    const QJsonObject interrupt = CodexProtocol::interruptParams(u"th-1"_s, u"tu-1"_s);
    CHECK(interrupt.value(u"threadId"_s).toString() == u"th-1"_s);
    CHECK(interrupt.value(u"turnId"_s).toString() == u"tu-1"_s);
}

TEST_CASE("account/read tells signed out, ChatGPT and other accounts apart, and keeps the email out", "[codex]") {
    const Result<CodexAccount> out = CodexProtocol::parseAccount(object(R"({"account":null,"requiresOpenaiAuth":true})"));
    REQUIRE(out.isOk());
    CHECK(out.value().kind == CodexAccount::Kind::SignedOut);
    CHECK(CodexProtocol::statusOf(out.value()).state == SignInStatus::State::SignedOut);

    const Result<CodexAccount> chat = CodexProtocol::parseAccount(
        object(R"({"account":{"type":"chatgpt","email":"someone@example.com","planType":"free"},"requiresOpenaiAuth":true})"));
    REQUIRE(chat.isOk());
    CHECK(chat.value().kind == CodexAccount::Kind::ChatGpt);
    const SignInStatus status = CodexProtocol::statusOf(chat.value());
    CHECK(status.state == SignInStatus::State::SignedIn);
    CHECK(status.detail == u"ChatGPT · Free"_s);
    CHECK_FALSE(status.detail.contains(u"someone"_s));

    const SignInStatus plus = CodexProtocol::statusOf(
        CodexProtocol::parseAccount(object(R"({"account":{"type":"chatgpt","email":null,"planType":"plus"}})")).value());
    CHECK(plus.detail == u"ChatGPT · Plus"_s);

    const SignInStatus key = CodexProtocol::statusOf(
        CodexProtocol::parseAccount(object(R"({"account":{"type":"apiKey"}})")).value());
    CHECK(key.state == SignInStatus::State::SignedIn);
    CHECK(key.detail == u"API key"_s);
}

TEST_CASE("An unknown plan is shown as the bare ChatGPT, never as the raw word", "[codex]") {
    const SignInStatus status = CodexProtocol::statusOf(CodexProtocol::parseAccount(
        object(R"({"account":{"type":"chatgpt","email":"a@b.c","planType":"someone@else.example"}})")).value());
    CHECK(status.detail == u"ChatGPT"_s);
    CHECK(CodexProtocol::planLabel(u"prolite"_s) == u"Pro Lite"_s);
    CHECK(CodexProtocol::planLabel(u"enterprise"_s) == u"Enterprise"_s);
    CHECK(CodexProtocol::planLabel(u"unknown"_s).isEmpty());
}

TEST_CASE("account/read without an account key is a parse error, not 'signed out'", "[codex]") {
    const Result<CodexAccount> result = CodexProtocol::parseAccount(object(R"({"requiresOpenaiAuth":true})"));
    REQUIRE_FALSE(result.isOk());
    CHECK(result.error().code == ErrorCode::Parse);
    CHECK_FALSE(CodexProtocol::parseAccount(object(R"({"account":{"type":42}})")).isOk());
}

TEST_CASE("account/login/start gives the login id and an https address", "[codex]") {
    const Result<CodexLogin> login = CodexProtocol::parseLoginStart(
        object(R"({"type":"chatgpt","loginId":"abc","authUrl":"https://auth.openai.com/oauth/authorize?x=1"})"));
    REQUIRE(login.isOk());
    CHECK(login.value().loginId == u"abc"_s);
    CHECK(login.value().authUrl.host() == u"auth.openai.com"_s);

    CHECK_FALSE(CodexProtocol::parseLoginStart(object(R"({"type":"chatgpt","loginId":"abc"})")).isOk());
    CHECK_FALSE(CodexProtocol::parseLoginStart(object(R"({"type":"chatgpt","authUrl":"https://a.example/"})")).isOk());
    // A link that is not a web page is never handed to a browser.
    CHECK_FALSE(CodexProtocol::parseLoginStart(
                    object(R"({"type":"chatgpt","loginId":"a","authUrl":"file:///etc/passwd"})")).isOk());
    CHECK_FALSE(CodexProtocol::parseLoginStart(
                    object(R"({"type":"chatgptDeviceCode","loginId":"a","verificationUrl":"https://a.example/"})")).isOk());
}

TEST_CASE("model/list keeps the visible models, in order, and says when there is more", "[codex]") {
    const QJsonObject list = object(R"({"data":[
        {"id":"a","model":"gpt-a","hidden":false},
        {"id":"b","model":"gpt-b","hidden":true},
        {"id":"c","model":"gpt-c"},
        {"id":"d","hidden":false}],"nextCursor":"page2"})");
    CHECK(CodexProtocol::parseModels(list) == QStringList{u"gpt-a"_s, u"gpt-c"_s, u"d"_s});
    CHECK(CodexProtocol::nextCursor(list) == u"page2"_s);
    CHECK(CodexProtocol::nextCursor(object(R"({"data":[],"nextCursor":null})")).isEmpty());
    CHECK(CodexProtocol::modelListParams(u"page2"_s).value(u"cursor"_s).toString() == u"page2"_s);
    CHECK_FALSE(CodexProtocol::modelListParams().contains(u"cursor"_s));
}

TEST_CASE("config/read names the MCP servers the user's config would start", "[codex]") {
    const QJsonObject config = object(R"({"config":{"mcp_servers":{"rea":{"command":"x"},"github":{"url":"https://a.example"}}}})");
    QStringList names = CodexProtocol::parseMcpServerNames(config);
    names.sort();
    CHECK(names == QStringList{u"github"_s, u"rea"_s});
    CHECK(CodexProtocol::parseMcpServerNames(object(R"({"config":{}})")).isEmpty());
}

TEST_CASE("thread/start and turn/start replies give their ids", "[codex]") {
    CHECK(CodexProtocol::parseThreadStart(object(R"({"thread":{"id":"th"},"model":"m"})")).value() == u"th"_s);
    CHECK_FALSE(CodexProtocol::parseThreadStart(object(R"({"model":"m"})")).isOk());
    CHECK(CodexProtocol::parseTurnStart(object(R"({"turn":{"id":"tu","status":"inProgress"}})")).value() == u"tu"_s);
    CHECK_FALSE(CodexProtocol::parseTurnStart(object(R"({"turn":{}})")).isOk());
}

// ---- CodexEvents ----

TEST_CASE("An agent message delta is text, with the item it belongs to", "[codex]") {
    const CodexEvent event = CodexEvents::decode(
        message(R"({"method":"item/agentMessage/delta","params":{"threadId":"t","turnId":"u","itemId":"i1","delta":"Hel"}})"));
    CHECK(event.kind == CodexEvent::Kind::Text);
    CHECK(event.text == u"Hel"_s);
    CHECK(event.itemId == u"i1"_s);
    CHECK(event.turnId == u"u"_s);
    CHECK(event.threadId == u"t"_s);
}

TEST_CASE("Reasoning deltas and item boundaries are activity", "[codex]") {
    for (const char *method : {"item/reasoning/summaryTextDelta", "item/reasoning/textDelta", "item/reasoning/summaryPartAdded"}) {
        const QByteArray line = QByteArray(R"({"method":")") + method + R"(","params":{"threadId":"t","turnId":"u","delta":"x"}})";
        CHECK(CodexEvents::decode(CodexProtocol::decode(line)).kind == CodexEvent::Kind::Activity);
    }
    const CodexEvent started = CodexEvents::decode(message(
        R"({"method":"item/started","params":{"threadId":"t","turnId":"u","item":{"type":"reasoning","id":"r"}}})"));
    CHECK(started.kind == CodexEvent::Kind::Activity);
    const CodexEvent userMessage = CodexEvents::decode(message(
        R"({"method":"item/started","params":{"threadId":"t","turnId":"u","item":{"type":"userMessage","id":"m"}}})"));
    CHECK(userMessage.kind == CodexEvent::Kind::Activity);
}

TEST_CASE("An item that is one of Codex's own tools is flagged", "[codex]") {
    for (const char *type : {"commandExecution", "fileChange", "mcpToolCall", "webSearch", "imageView", "sleep",
                             "imageGeneration", "dynamicToolCall", "collabAgentToolCall", "somethingNewInThe0170"}) {
        const QByteArray line = QByteArray(R"({"method":"item/started","params":{"threadId":"t","turnId":"u","item":{"type":")")
            + type + R"(","id":"x"}}})";
        const CodexEvent event = CodexEvents::decode(CodexProtocol::decode(line));
        INFO("type " << type);
        CHECK(event.kind == CodexEvent::Kind::OwnTool);
        CHECK(event.itemType == QString::fromLatin1(type));
    }
    for (const QString &harmless : {u"agentMessage"_s, u"userMessage"_s, u"reasoning"_s, u"plan"_s, u"contextCompaction"_s,
                                    u"hookPrompt"_s}) {
        CHECK_FALSE(CodexEvents::isOwnToolItem(harmless));
    }
}

TEST_CASE("A finished agent message carries its whole text", "[codex]") {
    const CodexEvent event = CodexEvents::decode(message(
        R"({"method":"item/completed","params":{"threadId":"t","turnId":"u","item":{"type":"agentMessage","id":"i9","text":"All of it","phase":"final_answer"}}})"));
    CHECK(event.kind == CodexEvent::Kind::ItemText);
    CHECK(event.text == u"All of it"_s);
    CHECK(event.itemId == u"i9"_s);
}

TEST_CASE("turn/completed carries the status and the error", "[codex]") {
    const CodexEvent done = CodexEvents::decode(message(
        R"({"method":"turn/completed","params":{"threadId":"t","turn":{"id":"u","status":"completed","error":null}}})"));
    CHECK(done.kind == CodexEvent::Kind::TurnDone);
    CHECK(done.status == CodexTurnStatus::Completed);
    CHECK(done.turnId == u"u"_s);
    CHECK(done.threadId == u"t"_s);

    const CodexEvent failed = CodexEvents::decode(message(
        R"({"method":"turn/completed","params":{"threadId":"t","turn":{"id":"u","status":"failed","error":{"message":"out of quota","codexErrorInfo":"usageLimitExceeded"}}}})"));
    CHECK(failed.status == CodexTurnStatus::Failed);
    CHECK(failed.errorMessage == u"out of quota"_s);
    CHECK(failed.errorInfo.toString() == u"usageLimitExceeded"_s);

    CHECK(CodexEvents::decode(message(
              R"({"method":"turn/completed","params":{"threadId":"t","turn":{"id":"u","status":"interrupted"}}})")).status
          == CodexTurnStatus::Interrupted);
}

TEST_CASE("The error notification says whether Codex will retry", "[codex]") {
    const CodexEvent event = CodexEvents::decode(message(
        R"({"method":"error","params":{"threadId":"t","turnId":"u","willRetry":true,"error":{"message":"stream dropped","codexErrorInfo":null}}})"));
    CHECK(event.kind == CodexEvent::Kind::ErrorNotice);
    CHECK(event.willRetry);
    CHECK(event.errorMessage == u"stream dropped"_s);
}

TEST_CASE("Login notifications", "[codex]") {
    const CodexEvent done = CodexEvents::decode(message(
        R"({"method":"account/login/completed","params":{"loginId":"L1","success":true,"error":null}})"));
    CHECK(done.kind == CodexEvent::Kind::LoginCompleted);
    CHECK(done.loginId == u"L1"_s);
    CHECK(done.ok);
    const CodexEvent failed = CodexEvents::decode(message(
        R"({"method":"account/login/completed","params":{"loginId":null,"success":false,"error":"denied"}})"));
    CHECK_FALSE(failed.ok);
    CHECK(failed.errorMessage == u"denied"_s);
    CHECK(CodexEvents::decode(message(R"({"method":"account/updated","params":{"authMode":"chatgpt"}})")).kind
          == CodexEvent::Kind::AccountUpdated);
}

TEST_CASE("Notifications Nexa does not use are ignored", "[codex]") {
    CHECK(CodexEvents::decode(message(R"({"method":"thread/started","params":{}})")).kind == CodexEvent::Kind::Ignored);
    CHECK(CodexEvents::decode(message(R"({"method":"mcpServer/startupStatus/updated","params":{}})")).kind
          == CodexEvent::Kind::Ignored);
    CHECK(CodexEvents::decode(message(R"({"id":1,"result":{}})")).kind == CodexEvent::Kind::Ignored);
}

TEST_CASE("Not being signed in is Unauthorized, with the way out in the message", "[codex]") {
    const Error error = CodexEvents::turnError(u"Please log in"_s, QJsonValue(u"unauthorized"_s));
    CHECK(error.code == ErrorCode::Unauthorized);
    CHECK(error.message == u"Codex is not signed in — sign in under Settings → Providers"_s);
    CHECK(CodexEvents::notSignedInMessage() == error.message);
}

TEST_CASE("A usage limit reads as HTTP 429, which the provider chain moves on from", "[codex]") {
    for (const char *name : {"usageLimitExceeded", "rateLimitExceeded", "serverOverloaded"}) {
        const Error error = CodexEvents::turnError(u"You hit your limit"_s, QJsonValue(QString::fromLatin1(name)));
        INFO(name);
        CHECK(error.code == ErrorCode::Http);
        CHECK(error.message.startsWith(u"HTTP 429: Codex: "_s));
        CHECK(error.message.contains(u"You hit your limit"_s));
    }
    // With no code from Codex, the words still say it.
    CHECK(CodexEvents::turnError(u"You've reached your usage limit. Try again later."_s, QJsonValue()).message
              .startsWith(u"HTTP 429"_s));
}

TEST_CASE("A connection that failed carries its status code when Codex has one", "[codex]") {
    const QJsonValue five = QJsonObject{{u"httpConnectionFailed"_s, QJsonObject{{u"httpStatusCode"_s, 503}}}};
    CHECK(CodexEvents::turnError(u"upstream down"_s, five).message.startsWith(u"HTTP 503: Codex: "_s));
    const QJsonValue none = QJsonObject{{u"responseStreamDisconnected"_s, QJsonObject{{u"httpStatusCode"_s, QJsonValue()}}}};
    CHECK(CodexEvents::turnError(u"dropped"_s, none).code == ErrorCode::Network);
    const QJsonValue four = QJsonObject{{u"httpConnectionFailed"_s, QJsonObject{{u"httpStatusCode"_s, 400}}}};
    CHECK(CodexEvents::turnError(u"bad"_s, four).code == ErrorCode::Network);
}

TEST_CASE("A model Codex does not have is NotFound, naming it by Codex's own words", "[codex]") {
    const Error error = CodexEvents::turnError(u"The model `gpt-9` does not exist or you do not have access to it."_s,
                                               QJsonValue(u"badRequest"_s));
    CHECK(error.code == ErrorCode::NotFound);
    CHECK(error.message.contains(u"gpt-9"_s));
}

TEST_CASE("Any other failure is Unknown with Codex's message", "[codex]") {
    const Error error = CodexEvents::turnError(u"Something odd"_s, QJsonValue(u"internalServerError"_s));
    CHECK(error.code == ErrorCode::Unknown);
    CHECK(error.message == u"Codex: Something odd"_s);
    CHECK(CodexEvents::turnError({}, QJsonValue()).message == u"Codex: the turn failed"_s);
}

TEST_CASE("JSON-RPC errors are mapped the same way", "[codex]") {
    CHECK(CodexEvents::rpcError(-32600, u"model not found: gpt-9"_s).code == ErrorCode::NotFound);
    CHECK(CodexEvents::rpcError(-32600, u"thread/start.dynamicTools requires experimentalApi capability"_s).code
          == ErrorCode::Unknown);
}

TEST_CASE("A process that ended says why in one line", "[codex]") {
    const Error error = CodexEvents::exitError(3, u"booting\nthread panicked at src/x.rs: oops\n"_s);
    CHECK(error.code == ErrorCode::ConnectionRefused);
    CHECK(error.message.contains(u"exit 3"_s));
    CHECK(error.message.contains(u"oops"_s));
    CHECK_FALSE(error.message.contains(u"booting"_s));
    CHECK(CodexEvents::exitError(1, {}).message.contains(u"no message"_s));
    CHECK(CodexEvents::exitError(1, u"Not logged in. Run codex login"_s).code == ErrorCode::Unauthorized);
    // One long line is cut.
    CHECK(CodexEvents::exitError(1, QString(2000, u'x')).message.size() < 400);
}

TEST_CASE("What Codex said is safe to show: no web address, email or token-like string survives", "[codex]") {
    const QString tidy = CodexEvents::tidy(
        u"Sorry jane.doe@example.com, sign in at https://auth.example.invalid/x?code=SECRET-CODE "
        "with token sk-proj-AbCdEf0123456789AbCdEf0123456789XyZ or eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.abc"_s);
    CHECK_FALSE(tidy.contains(u"jane.doe"_s));
    CHECK_FALSE(tidy.contains(u"@"_s));
    CHECK_FALSE(tidy.contains(u"SECRET-CODE"_s));
    CHECK_FALSE(tidy.contains(u"AbCdEf0123456789"_s));
    CHECK_FALSE(tidy.contains(u"eyJhbGciOi"_s));
    CHECK(tidy.contains(u"sign in at"_s));
    // Ordinary words, model names and short ids are left alone.
    CHECK(CodexEvents::tidy(u"The model gpt-5.1-codex-max does not exist (request 4f2a)"_s)
          == u"The model gpt-5.1-codex-max does not exist (request 4f2a)"_s);
    // And it reaches every message Nexa builds from Codex's words.
    CHECK_FALSE(CodexEvents::turnError(u"jane.doe@example.com hit a limit"_s, QJsonValue(u"other"_s)).message
                    .contains(u"jane.doe"_s));
    CHECK_FALSE(CodexEvents::rpcError(-32600, u"account jane.doe@example.com is odd"_s).message.contains(u"jane.doe"_s));
    CHECK_FALSE(CodexEvents::exitError(2, u"fatal: jane.doe@example.com\n"_s).message.contains(u"jane.doe"_s));
}

TEST_CASE("Codex's own tools that are only suggestions are off too", "[codex]") {
    const QStringList arguments = CodexProtocol::serverArguments();
    const qsizetype at = arguments.indexOf(u"tool_suggest"_s);
    REQUIRE(at > 0);
    CHECK(arguments.at(at - 1) == u"--disable"_s);
}

// codex-cli 0.166 with a broken ~/.codex/config.toml: the account answers, but every step that loads the file fails.
TEST_CASE("Codex's own settings file being invalid is named, with the line, and the chain moves on", "[codex]") {
    const QString home = QDir::homePath();
    const Error turn = CodexEvents::rpcError(
        -32603, u"failed to load workspace requirements: invalid configuration at %1/.codex/config.toml:149:9"_s.arg(home));
    CHECK(turn.code == ErrorCode::InvalidArgument);
    CHECK(turn.message.contains(u"~/.codex/config.toml"_s));
    CHECK(turn.message.contains(u"line 149"_s));
    CHECK_FALSE(turn.message.contains(home));
    CHECK(ProviderChain::meansUnavailable(turn));

    const Error exited = CodexEvents::exitError(
        1, u"Error loading configuration: /tmp/x/config.toml:12:3: unclosed table, expected `]`\n"_s);
    CHECK(exited.code == ErrorCode::InvalidArgument);
    CHECK(exited.message.contains(u"/tmp/x/config.toml"_s));
    CHECK(exited.message.contains(u"line 12"_s));

    const Error bootstrap = CodexEvents::turnError(u"failed to load bootstrap configuration"_s, QJsonValue());
    CHECK(bootstrap.code == ErrorCode::InvalidArgument);
    CHECK(bootstrap.message.contains(u"config.toml"_s));

    CHECK(CodexEvents::configTrouble(u"something else went wrong"_s).isEmpty());
    CHECK(CodexEvents::turnError(u"the model is thinking too hard"_s, QJsonValue()).code == ErrorCode::Unknown);
}
