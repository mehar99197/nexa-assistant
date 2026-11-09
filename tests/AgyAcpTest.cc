// core/ai/AgyAcp: agy's Agent Client Protocol (`agy acp`), as measured against agy 1.4.12 on 2026-10-31.

#include "core/ai/AgyAcp.h"

#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <catch2/catch_test_macros.hpp>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

// The reply `initialize` really gave (trimmed of the capabilities that do not matter here).
const QByteArray kInitializeReply =
    R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":1,"agentInfo":{"name":"Antigravity ACP Server","version":"1.4.12"},)"
    R"("agentCapabilities":{"loadSession":true,"sessionCapabilities":{"resume":{},"close":{},"load":{},"list":{}},)"
    R"("auth":{"logout":{}},"_meta":{"antigravity":{"tier":true,"systemPrompt":{"append":true}}}},)"
    R"("authMethods":[{"id":"oauth-personal","name":"Log in with Google"},)"
    R"({"id":"oauth-business","name":"Log in with Gemini Enterprise"},)"
    R"({"id":"gemini-api-key","name":"Gemini API key"},)"
    R"({"id":"agent-platform","name":"Gemini Enterprise Agent Platform"}]}})";

const QByteArray kSessionReply =
    R"({"jsonrpc":"2.0","id":2,"result":{"sessionId":"c0ffee","models":{"availableModels":[{"modelId":"gemini-3.8-flash-low"}],)"
    R"("currentModelId":"gemini-3.8-flash-low"},"_meta":{"antigravity":{"tier":{"name":"Google AI Pro","id":"g1-pro-tier",)"
    R"("upgradeUri":"https://example.invalid/upgrade"}}}}})";

QJsonObject object(const QByteArray &line) {
    return QJsonDocument::fromJson(line).object();
}

} // namespace

TEST_CASE("A request is one compact JSON-RPC line", "[agy-acp]") {
    const QByteArray line = AgyAcp::request(7, u"session/close"_s, QJsonObject{{u"sessionId"_s, u"abc"_s}});
    REQUIRE(line.endsWith('\n'));
    CHECK(line.count('\n') == 1);
    const QJsonObject json = object(line);
    CHECK(json.value(u"jsonrpc"_s).toString() == u"2.0"_s);
    CHECK(json.value(u"id"_s).toInt() == 7);
    CHECK(json.value(u"method"_s).toString() == u"session/close"_s);
    CHECK(json.value(u"params"_s).toObject().value(u"sessionId"_s).toString() == u"abc"_s);
    CHECK_FALSE(line.contains(' '));
}

TEST_CASE("A request with no parameters still carries an empty params object", "[agy-acp]") {
    const QJsonObject json = object(AgyAcp::request(3, u"logout"_s));
    CHECK(json.value(u"params"_s).isObject());
    CHECK(json.value(u"params"_s).toObject().isEmpty());
}

TEST_CASE("initialize asks for protocol 1 and says who Nexa is", "[agy-acp]") {
    const QJsonObject json = object(AgyAcp::initializeRequest(1));
    CHECK(json.value(u"method"_s).toString() == u"initialize"_s);
    const QJsonObject params = json.value(u"params"_s).toObject();
    CHECK(params.value(u"protocolVersion"_s).toInt() == 1);
    CHECK(params.value(u"clientCapabilities"_s).isObject());
    CHECK(params.value(u"clientCapabilities"_s).toObject().isEmpty());
    CHECK(params.value(u"clientInfo"_s).toObject().value(u"name"_s).toString() == u"nexa"_s);
    CHECK_FALSE(params.value(u"clientInfo"_s).toObject().value(u"version"_s).toString().isEmpty());
}

TEST_CASE("session/new carries the workspace and no MCP servers, session/close the id", "[agy-acp]") {
    const QJsonObject open = object(AgyAcp::newSessionRequest(2, u"/tmp/work"_s));
    CHECK(open.value(u"method"_s).toString() == u"session/new"_s);
    CHECK(open.value(u"params"_s).toObject().value(u"cwd"_s).toString() == u"/tmp/work"_s);
    REQUIRE(open.value(u"params"_s).toObject().value(u"mcpServers"_s).isArray());
    CHECK(open.value(u"params"_s).toObject().value(u"mcpServers"_s).toArray().isEmpty());

    const QJsonObject close = object(AgyAcp::closeSessionRequest(3, u"c0ffee"_s));
    CHECK(close.value(u"method"_s).toString() == u"session/close"_s);
    CHECK(close.value(u"params"_s).toObject().value(u"sessionId"_s).toString() == u"c0ffee"_s);
}

TEST_CASE("authenticate names the method, logout takes nothing", "[agy-acp]") {
    const QJsonObject auth = object(AgyAcp::authenticateRequest(2, u"oauth-personal"_s));
    CHECK(auth.value(u"method"_s).toString() == u"authenticate"_s);
    CHECK(auth.value(u"params"_s).toObject().value(u"methodId"_s).toString() == u"oauth-personal"_s);
    CHECK(object(AgyAcp::logoutRequest(4)).value(u"method"_s).toString() == u"logout"_s);
}

TEST_CASE("An error reply to a request from agy keeps the id as agy sent it", "[agy-acp]") {
    const QJsonObject numeric = object(AgyAcp::errorReply(99, AgyAcp::kMethodNotFoundCode, u"Method not found"_s));
    CHECK(numeric.value(u"id"_s).toInt() == 99);
    CHECK(numeric.value(u"error"_s).toObject().value(u"code"_s).toInt() == -32601);
    const QJsonObject text = object(AgyAcp::errorReply(u"req-1"_s, AgyAcp::kMethodNotFoundCode, u"no"_s));
    CHECK(text.value(u"id"_s).toString() == u"req-1"_s);
}

TEST_CASE("decode reads a response, an error, a notification and a request from agy", "[agy-acp]") {
    const AcpMessage response = AgyAcp::decode(kInitializeReply);
    REQUIRE(response.valid);
    CHECK(response.isResponse());
    CHECK(response.id == 1);
    CHECK_FALSE(response.isError);
    CHECK(response.result.isObject());

    const AcpMessage error = AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Authentication required"}})");
    REQUIRE(error.valid);
    CHECK(error.isResponse());
    CHECK(error.isError);
    CHECK(error.code == -32000);
    CHECK(error.message == u"Authentication required"_s);

    const AcpMessage notification = AgyAcp::decode(
        R"({"jsonrpc":"2.0","method":"session/update","params":{"sessionId":"x"}})");
    REQUIRE(notification.valid);
    CHECK(notification.isNotification());
    CHECK_FALSE(notification.isResponse());
    CHECK(notification.method == u"session/update"_s);

    const AcpMessage request = AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":"r1","method":"fs/read_text_file","params":{"path":"/x"}})");
    REQUIRE(request.valid);
    CHECK(request.isRequest());
    CHECK(request.rawId.toString() == u"r1"_s);
    CHECK(request.id == -1);
}

TEST_CASE("decode never calls what it cannot read a message", "[agy-acp]") {
    CHECK_FALSE(AgyAcp::decode("").valid);
    CHECK_FALSE(AgyAcp::decode("   ").valid);
    CHECK_FALSE(AgyAcp::decode("not json at all").valid);
    CHECK_FALSE(AgyAcp::decode("[1,2,3]").valid);
    CHECK_FALSE(AgyAcp::decode("\"text\"").valid);
    CHECK_FALSE(AgyAcp::decode("{}").valid);                              // neither id nor method
    CHECK_FALSE(AgyAcp::decode(R"({"jsonrpc":"2.0","id":1})").valid);     // no result, no error, no method
    CHECK_FALSE(AgyAcp::decode(R"({"jsonrpc":"2.0","result":{}})").valid);  // a result with no id
}

TEST_CASE("decode takes a line with a trailing carriage return or newline", "[agy-acp]") {
    const AcpMessage message = AgyAcp::decode(R"({"jsonrpc":"2.0","id":5,"result":{}})" "\r\n");
    REQUIRE(message.valid);
    CHECK(message.id == 5);
    CHECK(message.result.isObject());
}

TEST_CASE("parseInitialize reads the methods, the logout capability and the version", "[agy-acp]") {
    const AcpMessage message = AgyAcp::decode(kInitializeReply);
    const std::optional<AcpInitialize> info = AgyAcp::parseInitialize(message.result);
    REQUIRE(info);
    CHECK(info->version == u"1.4.12"_s);
    CHECK(info->canLogout);
    REQUIRE(info->authMethods.size() == 4);
    CHECK(info->authMethods.at(0).id == u"oauth-personal"_s);
    CHECK(info->authMethods.at(0).name == u"Log in with Google"_s);
    CHECK(info->authMethods.at(3).id == u"agent-platform"_s);
}

TEST_CASE("Without an auth.logout capability agy cannot sign out", "[agy-acp]") {
    const std::optional<AcpInitialize> none = AgyAcp::parseInitialize(
        QJsonDocument::fromJson(R"({"agentInfo":{"version":"2"},"agentCapabilities":{"loadSession":true},"authMethods":[]})")
            .object());
    REQUIRE(none);
    CHECK_FALSE(none->canLogout);
    const std::optional<AcpInitialize> off = AgyAcp::parseInitialize(
        QJsonDocument::fromJson(R"({"agentCapabilities":{"auth":{"logout":false}},"authMethods":[]})").object());
    REQUIRE(off);
    CHECK_FALSE(off->canLogout);
}

TEST_CASE("parseInitialize refuses what is not an initialize reply", "[agy-acp]") {
    CHECK_FALSE(AgyAcp::parseInitialize(QJsonValue()));
    CHECK_FALSE(AgyAcp::parseInitialize(QJsonValue(u"text"_s)));
    CHECK_FALSE(AgyAcp::parseInitialize(QJsonObject{}));
    CHECK_FALSE(AgyAcp::parseInitialize(QJsonObject{{u"unrelated"_s, 1}}));
}

TEST_CASE("parseInitialize skips auth methods that have no id", "[agy-acp]") {
    const std::optional<AcpInitialize> info = AgyAcp::parseInitialize(
        QJsonDocument::fromJson(R"({"authMethods":[{"name":"no id"},"junk",{"id":"oauth-personal","name":"G"}]})")
            .object());
    REQUIRE(info);
    REQUIRE(info->authMethods.size() == 1);
    CHECK(info->authMethods.first().id == u"oauth-personal"_s);
}

TEST_CASE("parseSession reads the plan, never an account", "[agy-acp]") {
    const AcpMessage message = AgyAcp::decode(kSessionReply);
    const std::optional<AcpSession> session = AgyAcp::parseSession(message.result);
    REQUIRE(session);
    CHECK(session->sessionId == u"c0ffee"_s);
    CHECK(session->plan == u"Google AI Pro"_s);
    CHECK(session->planId == u"g1-pro-tier"_s);
}

TEST_CASE("A session without a tier is still a signed-in session, without a plan", "[agy-acp]") {
    const std::optional<AcpSession> session =
        AgyAcp::parseSession(QJsonDocument::fromJson(R"({"sessionId":"s1","models":{}})").object());
    REQUIRE(session);
    CHECK(session->sessionId == u"s1"_s);
    CHECK(session->plan.isEmpty());
}

TEST_CASE("parseSession refuses a result with no session id", "[agy-acp]") {
    CHECK_FALSE(AgyAcp::parseSession(QJsonValue()));
    CHECK_FALSE(AgyAcp::parseSession(QJsonObject{}));
    CHECK_FALSE(AgyAcp::parseSession(QJsonObject{{u"sessionId"_s, 5}}));
    CHECK_FALSE(AgyAcp::parseSession(QJsonObject{{u"sessionId"_s, QString()}}));
    CHECK_FALSE(AgyAcp::parseSession(QJsonObject{{u"_meta"_s, QJsonObject{}}}));
}

TEST_CASE("Only an authentication error from session/new means signed out", "[agy-acp]") {
    CHECK(AgyAcp::isAuthRequired(AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Authentication required"}})")));
    CHECK(AgyAcp::isAuthRequired(AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"authentication required: log in"}})")));
    // Anything else — another code, another -32000 — is not "signed out", it is a failure to understand.
    CHECK_FALSE(AgyAcp::isAuthRequired(AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32000,"message":"Internal error"}})")));
    CHECK_FALSE(AgyAcp::isAuthRequired(AgyAcp::decode(
        R"({"jsonrpc":"2.0","id":2,"error":{"code":-32602,"message":"Authentication required"}})")));
    CHECK_FALSE(AgyAcp::isAuthRequired(AgyAcp::decode(kSessionReply)));
    CHECK_FALSE(AgyAcp::isAuthRequired(AcpMessage{}));
}

TEST_CASE("oauthMethods keeps the browser sign-ins and labels them for Settings", "[agy-acp]") {
    const std::optional<AcpInitialize> info = AgyAcp::parseInitialize(AgyAcp::decode(kInitializeReply).result);
    REQUIRE(info);
    const QList<SignInMethod> methods = AgyAcp::oauthMethods(*info);
    REQUIRE(methods.size() == 2);
    CHECK(methods.at(0).id == u"oauth-personal"_s);
    CHECK(methods.at(0).label == u"Google account"_s);
    CHECK_FALSE(methods.at(0).needsCode);
    CHECK(methods.at(1).id == u"oauth-business"_s);
    CHECK(methods.at(1).label == u"Gemini Enterprise"_s);
}

TEST_CASE("A browser sign-in agy adds later is offered under the name agy gives it", "[agy-acp]") {
    AcpInitialize info;
    info.authMethods = {{u"oauth-workspace"_s, u"Log in with Workspace"_s}, {u"gemini-api-key"_s, u"Key"_s}};
    const QList<SignInMethod> methods = AgyAcp::oauthMethods(info);
    REQUIRE(methods.size() == 1);
    CHECK(methods.first().id == u"oauth-workspace"_s);
    CHECK(methods.first().label == u"Log in with Workspace"_s);
}

TEST_CASE("authLink finds a login link in an auth notification and nowhere else", "[agy-acp]") {
    const AcpMessage link = AgyAcp::decode(
        R"({"jsonrpc":"2.0","method":"auth/url","params":{"url":"https://accounts.example.invalid/o/oauth2/auth?x=1"}})");
    CHECK(AgyAcp::authLink(link) == QUrl(u"https://accounts.example.invalid/o/oauth2/auth?x=1"_s));

    const AcpMessage camel = AgyAcp::decode(
        R"({"jsonrpc":"2.0","method":"authenticate/progress","params":{"authUrl":"http://127.0.0.1:1234/login"}})");
    CHECK(AgyAcp::authLink(camel) == QUrl(u"http://127.0.0.1:1234/login"_s));

    // not about auth, not a link, not a web link, not a notification
    CHECK(AgyAcp::authLink(AgyAcp::decode(
              R"({"jsonrpc":"2.0","method":"session/update","params":{"url":"https://example.invalid/"}})"))
              .isEmpty());
    CHECK(AgyAcp::authLink(AgyAcp::decode(
              R"j({"jsonrpc":"2.0","method":"auth/url","params":{"url":"javascript:alert(1)"}})j"))
              .isEmpty());
    CHECK(AgyAcp::authLink(AgyAcp::decode(
              R"({"jsonrpc":"2.0","method":"auth/url","params":{"url":"file:///etc/passwd"}})"))
              .isEmpty());
    CHECK(AgyAcp::authLink(AgyAcp::decode(R"({"jsonrpc":"2.0","method":"auth/url","params":{}})")).isEmpty());
    CHECK(AgyAcp::authLink(AgyAcp::decode(
              R"({"jsonrpc":"2.0","id":1,"result":{"url":"https://example.invalid/"}})"))
              .isEmpty());
}

TEST_CASE("safeText drops addresses and token-like runs and cuts long text", "[agy-acp]") {
    CHECK(AgyAcp::safeText(u"Sign-in was cancelled"_s) == u"Sign-in was cancelled"_s);
    const QString withEmail = AgyAcp::safeText(u"cannot sign in as someone@example.com now"_s);
    CHECK_FALSE(withEmail.contains(u"someone"_s));
    CHECK_FALSE(withEmail.contains(u"@"_s));
    const QString withToken = AgyAcp::safeText(u"bad code 4/0AbCdEfGhIjKlMnOpQrStUvWxYz0123456789 given"_s);
    CHECK_FALSE(withToken.contains(u"AbCdEfGh"_s));
    CHECK(withToken.contains(u"bad code"_s));
    const QString longText = AgyAcp::safeText(u"word "_s.repeated(200));
    CHECK(longText.size() <= 201);
    CHECK(longText.endsWith(u"…"_s));
    CHECK(AgyAcp::safeText(u"  spaced \n out  "_s) == u"spaced out"_s);
}

TEST_CASE("safeText drops links and the values of secret-looking parameters", "[agy-acp]") {
    const QString link = AgyAcp::safeText(
        u"callback https://accounts.example/cb?state=abcdefghij1234&x=1 failed"_s);
    CHECK_FALSE(link.contains(u"abcdefghij1234"_s));
    CHECK_FALSE(link.contains(u"accounts.example"_s));
    CHECK(link.contains(u"failed"_s));
    const QString params = AgyAcp::safeText(u"rejected code=Zx81 state: Qw3rTy9 token=abc"_s);
    CHECK_FALSE(params.contains(u"Zx81"_s));
    CHECK_FALSE(params.contains(u"Qw3rTy9"_s));
    CHECK_FALSE(params.contains(u"abc"_s));
    const QString shortCode = AgyAcp::safeText(u"verification 8F3K9Q2M7XPL4ZNB needed, or 482913"_s);
    CHECK_FALSE(shortCode.contains(u"8F3K9Q2M7XPL4ZNB"_s));
    CHECK_FALSE(shortCode.contains(u"482913"_s));
    CHECK(AgyAcp::safeText(u"Authentication required (error code 32000)"_s)
          == u"Authentication required (error code 32000)"_s);
}
