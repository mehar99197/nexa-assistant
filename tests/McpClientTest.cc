// core/mcp: the MCP client over a server process (tests/fake_mcp_server.py) and over HTTP (the fake
// server), and the connectors that turn a server's tools into Nexa tools.

#include "core/mcp/McpClient.h"
#include "core/mcp/McpConnectors.h"
#include "core/mcp/McpTransport.h"
#include "core/net/HttpClient.h"
#include "core/tools/ToolRegistry.h"

#include "FakeHttpServer.h"
#include "QtStringMakers.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>

#include <catch2/catch_test_macros.hpp>

#include <optional>

using namespace nexa;
using namespace Qt::StringLiterals;

namespace {

QString python() {
    return QStandardPaths::findExecutable(u"python3"_s);
}

QString fakeServer() {
    return QStringLiteral(NEXA_TESTS_DIR "/fake_mcp_server.py");
}

McpServerConfig fakeConfig(const QString &id = u"fake"_s) {
    McpServerConfig server;
    server.id = id;
    server.name = u"Fake server"_s;
    server.command = python();
    server.arguments = {fakeServer()};
    server.environment = {u"FAKE_MCP_VERSION=7.7"_s};
    return server;
}

template <typename T>
bool waitFor(std::optional<T> &slot) {
    return QTest::qWaitFor([&slot] { return slot.has_value(); }, 10'000);
}

} // namespace

TEST_CASE("An MCP server on stdio: initialise, every page of tools, calls, errors, its own ping") {
    if (python().isEmpty()) SKIP("python3 is needed for the fake MCP server");
    McpServerConfig config = fakeConfig();
    config.environment.append(u"FAKE_MCP_PING=1"_s);
    McpClient client(McpConnectors::makeTransport(config, nullptr));
    QSignalSpy closed(&client, &McpClient::closed);

    std::optional<Result<McpClient::ServerInfo>> opened;
    client.open([&opened](Result<McpClient::ServerInfo> info) { opened = std::move(info); });
    REQUIRE(waitFor(opened));
    REQUIRE(opened->isOk());
    CHECK(opened->value().name == u"fake-mcp"_s);
    CHECK(opened->value().version == u"7.7"_s);         // the environment reached the process
    CHECK(opened->value().tools);
    CHECK(client.isReady());

    std::optional<Result<QList<McpClient::Tool>>> listed;
    client.listTools([&listed](Result<QList<McpClient::Tool>> tools) { listed = std::move(tools); });
    REQUIRE(waitFor(listed));
    REQUIRE(listed->isOk());
    REQUIRE(listed->value().size() == 3);                 // both pages
    CHECK(listed->value().at(0).name == u"echo"_s);
    CHECK(listed->value().at(0).inputSchema.value(u"required"_s).toArray().first().toString() == u"text"_s);
    CHECK(listed->value().at(2).description == u"Always fails"_s);

    std::optional<Result<QString>> echoed;
    client.callTool(u"echo"_s, QJsonObject{{u"text"_s, u"hi"_s}}, [&echoed](Result<QString> text) { echoed = std::move(text); });
    REQUIRE(waitFor(echoed));
    REQUIRE(echoed->isOk());
    CHECK(echoed->value() == u"echo: hi"_s);

    std::optional<Result<QString>> sum;
    client.callTool(u"add"_s, QJsonObject{{u"a"_s, 3}, {u"b"_s, 4}}, [&sum](Result<QString> text) { sum = std::move(text); });
    REQUIRE(waitFor(sum));
    CHECK(sum->value() == u"7"_s);

    // A tool that reports an error, and a tool the server does not have.
    std::optional<Result<QString>> failed;
    client.callTool(u"fail"_s, {}, [&failed](Result<QString> text) { failed = std::move(text); });
    REQUIRE(waitFor(failed));
    REQUIRE_FALSE(failed->isOk());
    CHECK(failed->error().message == u"boom"_s);
    std::optional<Result<QString>> unknown;
    client.callTool(u"nothing"_s, {}, [&unknown](Result<QString> text) { unknown = std::move(text); });
    REQUIRE(waitFor(unknown));
    REQUIRE_FALSE(unknown->isOk());
    CHECK(unknown->error().code == ErrorCode::InvalidArgument);
    CHECK(unknown->error().message.contains(u"Unknown tool"_s));
    CHECK(closed.isEmpty());        // its ping was answered, the link held

    // The process goes away: the client says so, and a call in flight comes back as an error.
    std::optional<Result<QString>> gone;
    client.callTool(u"quit"_s, {}, [&gone](Result<QString> text) { gone = std::move(text); });
    REQUIRE(waitFor(gone));
    CHECK_FALSE(gone->isOk());
    CHECK(QTest::qWaitFor([&closed] { return !closed.isEmpty(); }, 5000));
    CHECK_FALSE(client.isReady());
}

TEST_CASE("A program that is not there is reported, not waited for") {
    McpServerConfig config;
    config.id = u"missing"_s;
    config.command = u"/nonexistent/mcp-server"_s;
    McpClient client(McpConnectors::makeTransport(config, nullptr));
    std::optional<Result<McpClient::ServerInfo>> opened;
    client.open([&opened](Result<McpClient::ServerInfo> info) { opened = std::move(info); });
    REQUIRE(waitFor(opened));
    REQUIRE_FALSE(opened->isOk());
    CHECK(opened->error().code == ErrorCode::NotFound);
}

TEST_CASE("Over HTTP every message is a POST; the answer is JSON or a short event stream") {
    test::FakeHttpServer server;
    HttpClient http;
    McpServerConfig config;
    config.id = u"web"_s;
    config.transport = u"http"_s;
    config.url = server.baseUrl().toString() + u"/mcp"_s;
    config.headers = {u"Authorization: Bearer secret"_s};
    McpClient client(McpConnectors::makeTransport(config, &http));

    server.respond({200, "application/json",
                    {R"({"jsonrpc":"2.0","id":1,"result":{"protocolVersion":"2025-06-18","capabilities":{"tools":{}},)"
                     R"("serverInfo":{"name":"web-mcp","version":"2"}}})"}});
    std::optional<Result<McpClient::ServerInfo>> opened;
    client.open([&opened](Result<McpClient::ServerInfo> info) { opened = std::move(info); });
    REQUIRE(waitFor(opened));
    REQUIRE(opened->isOk());
    CHECK(opened->value().name == u"web-mcp"_s);
    CHECK(server.requestHead().contains("Authorization: Bearer secret"));
    CHECK(server.requestHead().contains("Accept: application/json, text/event-stream"));
    const QJsonObject sent = QJsonDocument::fromJson(server.requestBody()).object();
    CHECK(sent.value(u"method"_s).toString() == u"initialize"_s);
    CHECK(sent.value(u"params"_s).toObject().value(u"protocolVersion"_s).toString() == u"2025-06-18"_s);

    // The call's answer as an event stream, the way servers that stream progress reply.
    server.respond({200, "text/event-stream",
                    {"event: message\ndata: {\"jsonrpc\":\"2.0\",\"id\":2,", "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"from the web\"}]}}\n\n"}});
    std::optional<Result<QString>> answer;
    client.callTool(u"any"_s, {}, [&answer](Result<QString> text) { answer = std::move(text); });
    REQUIRE(waitFor(answer));
    REQUIRE(answer->isOk());
    CHECK(answer->value() == u"from the web"_s);

    // A server error becomes one of ours.
    server.respond({500, "application/json", {R"({"error":"down"})"}});
    std::optional<Result<QString>> down;
    client.callTool(u"any"_s, {}, [&down](Result<QString> text) { down = std::move(text); });
    REQUIRE(waitFor(down));
    CHECK_FALSE(down->isOk());

    // Pure: the shapes a body can take.
    CHECK(HttpMcpTransport::messagesIn(R"({"jsonrpc":"2.0","id":1,"result":{}})", "application/json").size() == 1);
    CHECK(HttpMcpTransport::messagesIn(R"([{"id":1,"result":{}},{"id":2,"result":{}}])", "application/json").size() == 2);
    CHECK(HttpMcpTransport::messagesIn("data: {\"id\":1}\n\ndata: {\"id\":2}\n\n", "text/event-stream; charset=utf-8").size() == 2);
    CHECK(HttpMcpTransport::messagesIn("", "application/json").isEmpty());
}

TEST_CASE("The result text of a call, and the tools in a list, are read as the protocol lays them out") {
    const QJsonObject mixed{{u"content"_s, QJsonArray{QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"one"_s}},
                                                      QJsonObject{{u"type"_s, u"image"_s}, {u"mimeType"_s, u"image/png"_s}},
                                                      QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"two"_s}}}}};
    CHECK(McpClient::resultText(mixed).value() == u"one\n[image: image/png]\ntwo"_s);
    CHECK(McpClient::resultText(QJsonObject{}).value() == u"(the tool returned nothing)"_s);
    const QJsonObject structured{{u"structuredContent"_s, QJsonObject{{u"ok"_s, true}}}};
    CHECK(McpClient::resultText(structured).value() == u"{\"ok\":true}"_s);
    const QJsonObject failed{{u"isError"_s, true}, {u"content"_s, QJsonArray{QJsonObject{{u"type"_s, u"text"_s}, {u"text"_s, u"nope"_s}}}}};
    CHECK(McpClient::resultText(failed).error().message == u"nope"_s);

    const QJsonObject list{{u"tools"_s, QJsonArray{QJsonObject{{u"name"_s, u"a"_s}, {u"title"_s, u"A tool"_s}},
                                                   QJsonObject{{u"description"_s, u"nameless"_s}}}}};
    const QList<McpClient::Tool> tools = McpClient::toolsIn(list);
    REQUIRE(tools.size() == 1);
    CHECK(tools.first().description == u"A tool"_s);

    CHECK(McpConnectors::toolName(u"My Server"_s, u"do:thing"_s) == u"mcp_My_Server_do_thing"_s);
    CHECK(McpConnectors::toolName(u"s"_s, QString(100, u'x')).size() == 64);
}

TEST_CASE("Connectors register a server's tools as Nexa tools and withdraw them when it goes") {
    if (python().isEmpty()) SKIP("python3 is needed for the fake MCP server");
    ToolRegistry registry;
    HttpClient http;
    McpConnectors connectors(&registry, &http);
    QSignalSpy status(&connectors, &McpConnectors::statusChanged);

    connectors.apply({fakeConfig()});
    REQUIRE(QTest::qWaitFor([&registry] { return registry.contains(u"mcp_fake_echo"_s); }, 10'000));
    CHECK(registry.contains(u"mcp_fake_add"_s));
    CHECK(registry.contains(u"mcp_fake_fail"_s));
    const ITool *echo = registry.find(u"mcp_fake_echo"_s);
    CHECK(echo->spec().description.startsWith(u"[Fake server] Says it back"_s));
    CHECK(echo->spec().privileged);
    CHECK(echo->spec().risk.contains(u"Fake server"_s));
    CHECK(echo->spec().parameters.value(u"type"_s).toString() == u"object"_s);
    REQUIRE(connectors.statuses().size() == 1);
    CHECK(connectors.statuses().first().state == u"ready"_s);
    CHECK(connectors.statuses().first().tools.size() == 3);

    std::optional<Result<ToolResult>> result;
    registry.find(u"mcp_fake_echo"_s)->invoke(QJsonObject{{u"text"_s, u"through the registry"_s}},
                                              [&result](Result<ToolResult> r) { result = std::move(r); });
    REQUIRE(waitFor(result));
    REQUIRE(result->isOk());
    CHECK(result->value().text == u"echo: through the registry"_s);

    // A probe for Settings → Test lists the tools without touching the registry.
    std::optional<Result<QStringList>> probed;
    McpConnectors::probe(fakeConfig(u"other"_s), &http, &connectors, [&probed](Result<QStringList> names) { probed = std::move(names); });
    REQUIRE(waitFor(probed));
    REQUIRE(probed->isOk());
    CHECK(probed->value() == QStringList{u"echo"_s, u"add"_s, u"fail"_s});
    CHECK_FALSE(registry.contains(u"mcp_other_echo"_s));

    // Disabled: off, tools gone. Removed: gone altogether.
    McpServerConfig disabled = fakeConfig();
    disabled.enabled = false;
    connectors.apply({disabled});
    CHECK_FALSE(registry.contains(u"mcp_fake_echo"_s));
    CHECK(connectors.statuses().first().state == u"off"_s);
    connectors.apply({});
    CHECK(connectors.statuses().isEmpty());
}
