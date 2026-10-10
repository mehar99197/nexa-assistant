#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QJsonObject>
#include <QObject>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <functional>

namespace nexa {

class HttpClient;

// One way of reaching an MCP server (Strategy): JSON-RPC messages go in, messages come out.
// McpClient speaks the protocol on top; the transport only carries it.
class IMcpTransport : public QObject {
    Q_OBJECT
public:
    using QObject::QObject;

    virtual void start(std::function<void(Result<void>)> done) = 0;    // the process is up, or the URL is set
    virtual void send(const QJsonObject &message) = 0;
    virtual void stop() = 0;
    virtual QString describe() const = 0;       // "python3 server.py" or the URL, for status lines

signals:
    void received(const QJsonObject &message);
    void failed(const nexa::Error &error);       // the link broke; nothing more will arrive
};

// A server run as a child process, messages as one JSON object per line on its standard streams
// (MCP's stdio transport). Its stderr is logged, never shown as a result.
class StdioMcpTransport : public IMcpTransport {
    Q_OBJECT
public:
    StdioMcpTransport(QString program, QStringList arguments, QStringList environment /* KEY=VALUE */,
                      QObject *parent = nullptr);
    ~StdioMcpTransport() override;

    void start(std::function<void(Result<void>)> done) override;
    void send(const QJsonObject &message) override;
    void stop() override;
    QString describe() const override;

private:
    void readLines();

    QString program_;
    QStringList arguments_;
    QStringList environment_;
    QProcess process_;
    QByteArray buffer_;
    bool stopping_{false};
};

// A server behind a URL (MCP's Streamable HTTP transport): every message is a POST; the answer
// comes back as JSON or as a short event stream; the session id the server hands out on
// initialize travels back in a header. Headers from the config (an Authorization, typically) go
// on every request.
class HttpMcpTransport : public IMcpTransport {
    Q_OBJECT
public:
    HttpMcpTransport(HttpClient *http, QString url, QStringList headers /* "Name: value" */, QObject *parent = nullptr);

    void start(std::function<void(Result<void>)> done) override;
    void send(const QJsonObject &message) override;
    void stop() override;
    QString describe() const override { return url_; }

    // Pure: the messages inside a response body — one object, an array, or SSE data lines.
    static QList<QJsonObject> messagesIn(const QByteArray &body, const QByteArray &contentType);

private:
    HttpClient *http_;
    QString url_;
    QStringList headers_;
    QByteArray sessionId_;
    bool stopped_{false};
};

} // namespace nexa
