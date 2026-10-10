#pragma once

#include "core/Result.h"

#include <QJsonObject>
#include <QList>
#include <QObject>
#include <QString>

#include <functional>
#include <memory>
#include <unordered_map>

class QTimer;

namespace nexa {

class IMcpTransport;

// A client of one MCP server (Model Context Protocol: JSON-RPC 2.0 over an IMcpTransport):
// initialise, list its tools, call one. Requests carry ids and time out; the server's own requests
// get a polite refusal (its ping is answered); its notice that the tool list changed is passed on.
// Nothing here blocks — every call takes a callback that runs exactly once.
class McpClient : public QObject {
    Q_OBJECT
public:
    struct Tool {
        QString name;
        QString description;
        QJsonObject inputSchema;    // a JSON Schema for the arguments object
    };
    struct ServerInfo {
        QString name;
        QString version;
        QString protocolVersion;
        bool tools{false};          // the server offers tools at all
    };
    static constexpr QLatin1StringView kProtocolVersion{"2025-06-18"};
    static constexpr int kRequestTimeoutMs = 30'000;
    static constexpr int kCallTimeoutMs = 120'000;

    explicit McpClient(std::unique_ptr<IMcpTransport> transport, QObject *parent = nullptr);
    ~McpClient() override;

    void open(std::function<void(Result<ServerInfo>)> done);        // start, initialize, initialized
    void listTools(std::function<void(Result<QList<Tool>>)> done);  // every page
    // The text the tool answered with; an error the tool reports comes back as an Error.
    void callTool(const QString &name, const QJsonObject &arguments, std::function<void(Result<QString>)> done);
    void close();
    bool isReady() const { return ready_; }
    const ServerInfo &server() const { return server_; }
    QString describe() const;

    // Pure: what a tools/call result says, and the tools a tools/list result holds.
    static Result<QString> resultText(const QJsonObject &result);
    static QList<Tool> toolsIn(const QJsonObject &result);

signals:
    void toolsChanged();                    // the server's tool list changed: list again
    void closed(const QString &reason);     // the link broke or close() was called

private:
    void request(const QString &method, const QJsonObject &params, int timeoutMs,
                 std::function<void(Result<QJsonObject>)> done);
    void notify(const QString &method, const QJsonObject &params = {});
    void onMessage(const QJsonObject &message);
    void listPage(const QString &cursor, QList<Tool> sofar, std::function<void(Result<QList<Tool>>)> done);
    void fail(const Error &error);

    struct Pending {
        std::function<void(Result<QJsonObject>)> done;
        QTimer *timer{nullptr};
    };

    std::unique_ptr<IMcpTransport> transport_;
    std::unordered_map<qint64, Pending> pending_;
    qint64 nextId_{1};
    bool ready_{false};
    bool closed_{false};
    ServerInfo server_;
};

} // namespace nexa
