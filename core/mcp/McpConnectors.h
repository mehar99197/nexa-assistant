#pragma once

#include "core/Result.h"
#include "core/config/Config.h"
#include "core/mcp/McpClient.h"
#include "core/tools/ITool.h"
#include "core/tools/ToolSpec.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>
#include <map>
#include <memory>
#include <optional>

namespace nexa {

class HttpClient;
class IMcpTransport;
class ToolRegistry;

// Keeps one McpClient per configured MCP server (Settings → Connectors) and turns the tools each
// lists into Nexa tools named mcp_<server>_<tool>, registered beside the PC's and the phone's, so
// the model sees them all alike; every call still goes through ToolExecutor — policy, confirmation,
// audit. A server that cannot be reached is reported, and tried again on the next apply().
// A built-in server (the browser) uses an instance of its own with a Shaping: its tools keep their
// own names, flags and risks of its choosing, and their arguments and results pass its filters.
// (Facade over the clients; Adapter from an MCP tool to an ITool.)
class McpConnectors : public QObject {
    Q_OBJECT
public:
    struct Status {
        QString id;
        QString name;
        QString state;          // off | connecting | ready | failed
        QString detail;         // "3 tools", or what went wrong
        QStringList tools;      // the Nexa names of its tools
    };

    // How a server's tools become Nexa's. Unset parts keep the default: mcp_<server>_<tool>,
    // privileged, the arguments and the result passed through as they are.
    struct Shaping {
        // The spec to register for a tool (the defaults given), or nothing to leave it out.
        std::function<std::optional<ToolSpec>(const McpClient::Tool &tool, ToolSpec defaults)> spec;
        std::function<QJsonObject(const QString &tool, QJsonObject arguments)> arguments;     // before each call
        std::function<ToolResult(const QString &tool, ToolResult result)> result;            // after it
    };

    McpConnectors(ToolRegistry *tools, HttpClient *http, QObject *parent = nullptr);
    ~McpConnectors() override;

    void setShaping(Shaping shaping);   // before apply()

    void apply(const QList<McpServerConfig> &servers);
    QList<Status> statuses() const;

    // Settings → Test: a client of its own that initialises, lists the tools, and goes away.
    static void probe(const McpServerConfig &server, HttpClient *http, QObject *context,
                      std::function<void(Result<QStringList>)> done);
    static std::unique_ptr<IMcpTransport> makeTransport(const McpServerConfig &server, HttpClient *http);
    // Pure: "mcp_<server>_<tool>" in the characters a tool name may use, at most 64 of them.
    static QString toolName(const QString &serverId, const QString &tool);

signals:
    void statusChanged(const QString &serverId, const QString &state, const QString &detail);

private:
    struct Link {
        McpServerConfig config;
        std::unique_ptr<McpClient> client;
        QStringList toolNames;
        Status status;
    };

    void connectLink(Link *link);
    void listAndAdopt(Link *link);
    void adoptTools(Link *link, const QList<McpClient::Tool> &tools);
    void dropTools(Link *link);
    void setState(Link *link, const QString &state, const QString &detail);
    bool isLive(const Link *link) const;

    ToolRegistry *registry_;
    HttpClient *http_;
    Shaping shaping_;
    std::map<QString, std::unique_ptr<Link>> links_;    // by server id; the pointers stay put
};

} // namespace nexa
