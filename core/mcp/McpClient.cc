#include "core/mcp/McpClient.h"

#include "core/Logging.h"
#include "core/mcp/McpTransport.h"

#include <QJsonArray>
#include <QJsonValue>
#include <QTimer>

#include <utility>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

ErrorCode codeFor(int jsonRpcCode) {
    switch (jsonRpcCode) {
    case -32601: return ErrorCode::NotSupported;    // method not found
    case -32602: return ErrorCode::InvalidArgument;
    case -32700: return ErrorCode::Parse;
    default: return ErrorCode::Unknown;
    }
}

} // namespace

McpClient::McpClient(std::unique_ptr<IMcpTransport> transport, QObject *parent)
    : QObject(parent), transport_(std::move(transport)) {
    connect(transport_.get(), &IMcpTransport::received, this, &McpClient::onMessage);
    connect(transport_.get(), &IMcpTransport::failed, this, &McpClient::fail);
}

McpClient::~McpClient() {
    close();
}

QString McpClient::describe() const {
    return transport_->describe();
}

void McpClient::open(std::function<void(Result<ServerInfo>)> done) {
    closed_ = false;
    transport_->start([this, done](Result<void> started) {
        if (!started) {
            done(started.error());
            return;
        }
        const QJsonObject params{{u"protocolVersion"_s, QString(kProtocolVersion)},
                                 {u"capabilities"_s, QJsonObject{}},
                                 {u"clientInfo"_s, QJsonObject{{u"name"_s, u"Nexa"_s}, {u"version"_s, u"0.1"_s}}}};
        request(u"initialize"_s, params, kRequestTimeoutMs, [this, done](Result<QJsonObject> result) {
            if (!result) {
                done(result.error());
                return;
            }
            const QJsonObject info = result.value().value(u"serverInfo"_s).toObject();
            server_.name = info.value(u"name"_s).toString();
            server_.version = info.value(u"version"_s).toString();
            server_.protocolVersion = result.value().value(u"protocolVersion"_s).toString();
            server_.tools = result.value().value(u"capabilities"_s).toObject().contains(u"tools"_s);
            ready_ = true;
            notify(u"notifications/initialized"_s);
            qCInfo(lcMcp).noquote() << "connected to" << server_.name << server_.version << "at" << describe();
            done(server_);
        });
    });
}

void McpClient::listTools(std::function<void(Result<QList<Tool>>)> done) {
    listPage({}, {}, std::move(done));
}

void McpClient::listPage(const QString &cursor, QList<Tool> sofar, std::function<void(Result<QList<Tool>>)> done) {
    QJsonObject params;
    if (!cursor.isEmpty()) params.insert(u"cursor"_s, cursor);
    request(u"tools/list"_s, params, kRequestTimeoutMs, [this, sofar, done](Result<QJsonObject> result) mutable {
        if (!result) {
            done(result.error());
            return;
        }
        sofar += toolsIn(result.value());
        const QString next = result.value().value(u"nextCursor"_s).toString();
        if (next.isEmpty() || sofar.size() > 500) {
            done(sofar);
            return;
        }
        listPage(next, std::move(sofar), std::move(done));
    });
}

void McpClient::callTool(const QString &name, const QJsonObject &arguments, std::function<void(Result<QString>)> done) {
    request(u"tools/call"_s, QJsonObject{{u"name"_s, name}, {u"arguments"_s, arguments}}, kCallTimeoutMs,
            [done](Result<QJsonObject> result) {
        if (!result) {
            done(result.error());
            return;
        }
        done(resultText(result.value()));
    });
}

void McpClient::close() {
    if (closed_) return;
    closed_ = true;
    ready_ = false;
    transport_->stop();
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &[id, call] : pending) {
        if (call.timer) call.timer->deleteLater();
        call.done(Error{ErrorCode::Cancelled, tr("The connector was closed.")});
    }
}

Result<QString> McpClient::resultText(const QJsonObject &result) {
    QStringList parts;
    for (const QJsonValue &item : result.value(u"content"_s).toArray()) {
        const QJsonObject content = item.toObject();
        const QString type = content.value(u"type"_s).toString();
        if (type == u"text"_s) {
            parts.append(content.value(u"text"_s).toString());
        } else if (type == u"image"_s || type == u"audio"_s) {
            parts.append(u"[%1: %2]"_s.arg(type, content.value(u"mimeType"_s).toString()));
        } else if (type == u"resource"_s || type == u"resource_link"_s) {
            const QJsonObject resource = content.value(u"resource"_s).toObject();
            const QString text = resource.value(u"text"_s).toString();
            parts.append(text.isEmpty() ? u"[resource: %1]"_s.arg(resource.value(u"uri"_s).toString(content.value(u"uri"_s).toString()))
                                        : text);
        }
    }
    QString text = parts.join(u'\n').trimmed();
    if (result.value(u"isError"_s).toBool()) {
        return Error{ErrorCode::Unknown, text.isEmpty() ? u"The tool reported an error."_s : text};
    }
    if (text.isEmpty() && result.contains(u"structuredContent"_s)) {
        text = QString::fromUtf8(QJsonDocument(result.value(u"structuredContent"_s).toObject()).toJson(QJsonDocument::Compact));
    }
    return text.isEmpty() ? u"(the tool returned nothing)"_s : text;
}

QList<McpClient::Tool> McpClient::toolsIn(const QJsonObject &result) {
    QList<Tool> tools;
    for (const QJsonValue &value : result.value(u"tools"_s).toArray()) {
        const QJsonObject object = value.toObject();
        Tool tool{object.value(u"name"_s).toString(), object.value(u"description"_s).toString(),
                  object.value(u"inputSchema"_s).toObject()};
        if (tool.description.isEmpty()) tool.description = object.value(u"title"_s).toString();
        if (!tool.name.isEmpty()) tools.append(tool);
    }
    return tools;
}

void McpClient::request(const QString &method, const QJsonObject &params, int timeoutMs,
                        std::function<void(Result<QJsonObject>)> done) {
    if (closed_) {
        done(Error{ErrorCode::Cancelled, tr("The connector is closed.")});
        return;
    }
    const qint64 id = nextId_++;
    auto *timer = new QTimer(this);
    timer->setSingleShot(true);
    connect(timer, &QTimer::timeout, this, [this, id, method] {
        const auto it = pending_.find(id);
        if (it == pending_.end()) return;
        Pending call = std::move(it->second);
        pending_.erase(it);
        call.timer->deleteLater();
        call.done(Error{ErrorCode::Timeout, tr("%1 did not answer %2 in time.").arg(describe(), method)});
    });
    pending_.emplace(id, Pending{std::move(done), timer});
    timer->start(timeoutMs);
    transport_->send(QJsonObject{{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"method"_s, method}, {u"params"_s, params}});
}

void McpClient::notify(const QString &method, const QJsonObject &params) {
    QJsonObject message{{u"jsonrpc"_s, u"2.0"_s}, {u"method"_s, method}};
    if (!params.isEmpty()) message.insert(u"params"_s, params);
    transport_->send(message);
}

void McpClient::onMessage(const QJsonObject &message) {
    const QJsonValue id = message.value(u"id"_s);
    const QString method = message.value(u"method"_s).toString();
    if (method.isEmpty()) {
        // An answer to one of ours.
        const auto it = pending_.find(qint64(id.toDouble(-1)));
        if (it == pending_.end()) return;
        Pending call = std::move(it->second);
        pending_.erase(it);
        if (call.timer) call.timer->deleteLater();
        if (message.contains(u"error"_s)) {
            const QJsonObject error = message.value(u"error"_s).toObject();
            call.done(Error{codeFor(error.value(u"code"_s).toInt()), error.value(u"message"_s).toString()});
        } else {
            call.done(message.value(u"result"_s).toObject());
        }
        return;
    }
    if (id.isUndefined() || id.isNull()) {
        // A notification from the server.
        if (method == u"notifications/tools/list_changed"_s) emit toolsChanged();
        return;
    }
    // A request from the server: ping is answered, nothing else is offered.
    if (method == u"ping"_s) {
        transport_->send(QJsonObject{{u"jsonrpc"_s, u"2.0"_s}, {u"id"_s, id}, {u"result"_s, QJsonObject{}}});
        return;
    }
    transport_->send(QJsonObject{{u"jsonrpc"_s, u"2.0"_s},
                                 {u"id"_s, id},
                                 {u"error"_s, QJsonObject{{u"code"_s, -32601}, {u"message"_s, u"Nexa does not offer this."_s}}}});
}

void McpClient::fail(const Error &error) {
    if (closed_) return;
    qCWarning(lcMcp).noquote() << describe() << "failed:" << error.message;
    closed_ = true;
    ready_ = false;
    auto pending = std::move(pending_);
    pending_.clear();
    for (auto &[id, call] : pending) {
        if (call.timer) call.timer->deleteLater();
        call.done(error);
    }
    emit closed(error.message);
}

} // namespace nexa
