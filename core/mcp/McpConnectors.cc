#include "core/mcp/McpConnectors.h"

#include "core/Logging.h"
#include "core/mcp/McpTransport.h"
#include "core/tools/FunctionTool.h"
#include "core/tools/ToolRegistry.h"
#include "core/tools/ToolSpec.h"

#include <QPointer>
#include <QRegularExpression>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

QString sanitized(const QString &text) {
    static const QRegularExpression other(u"[^A-Za-z0-9_-]+"_s);
    QString clean = text;
    clean.replace(other, u"_"_s);
    return clean;
}

} // namespace

McpConnectors::McpConnectors(ToolRegistry *tools, HttpClient *http, QObject *parent)
    : QObject(parent), registry_(tools), http_(http) {}

McpConnectors::~McpConnectors() {
    for (auto &[id, link] : links_) dropTools(link.get());
}

void McpConnectors::setShaping(Shaping shaping) {
    shaping_ = std::move(shaping);
}

QString McpConnectors::toolName(const QString &serverId, const QString &tool) {
    return (u"mcp_"_s + sanitized(serverId) + u'_' + sanitized(tool)).left(64);
}

std::unique_ptr<IMcpTransport> McpConnectors::makeTransport(const McpServerConfig &server, HttpClient *http) {
    if (server.transport == u"http"_s) return std::make_unique<HttpMcpTransport>(http, server.url, server.headers);
    return std::make_unique<StdioMcpTransport>(server.command, server.arguments, server.environment);
}

void McpConnectors::apply(const QList<McpServerConfig> &servers) {
    // Gone or changed: closed, their tools withdrawn.
    for (auto it = links_.begin(); it != links_.end();) {
        const auto wanted = std::find_if(servers.cbegin(), servers.cend(),
                                         [&it](const McpServerConfig &s) { return s.id == it->first; });
        if (wanted == servers.cend() || !(*wanted == it->second->config)) {
            dropTools(it->second.get());
            it = links_.erase(it);
        } else {
            ++it;
        }
    }
    // New or changed: connected. Unchanged but failed: tried again.
    for (const McpServerConfig &server : servers) {
        if (server.id.isEmpty()) continue;
        if (const auto existing = links_.find(server.id); existing != links_.end()) {
            Link *link = existing->second.get();
            if (server.enabled && link->status.state == u"failed"_s) connectLink(link);
            continue;
        }
        auto link = std::make_unique<Link>();
        link->config = server;
        link->status = Status{server.id, server.name, u"off"_s, {}, {}};
        Link *raw = link.get();
        links_.emplace(server.id, std::move(link));
        if (server.enabled) connectLink(raw);
    }
}

QList<McpConnectors::Status> McpConnectors::statuses() const {
    QList<Status> all;
    for (const auto &[id, link] : links_) all.append(link->status);
    return all;
}

bool McpConnectors::isLive(const Link *link) const {
    for (const auto &[id, candidate] : links_) {
        if (candidate.get() == link) return true;
    }
    return false;
}

void McpConnectors::setState(Link *link, const QString &state, const QString &detail) {
    link->status.state = state;
    link->status.detail = detail;
    emit statusChanged(link->config.id, state, detail);
}

void McpConnectors::connectLink(Link *link) {
    link->client = std::make_unique<McpClient>(makeTransport(link->config, http_));
    setState(link, u"connecting"_s, tr("Connecting to %1…").arg(link->client->describe()));
    McpClient *client = link->client.get();
    connect(client, &McpClient::toolsChanged, this, [this, link, client] {
        if (isLive(link) && link->client.get() == client) listAndAdopt(link);
    });
    connect(client, &McpClient::closed, this, [this, link, client](const QString &reason) {
        if (!isLive(link) || link->client.get() != client) return;
        dropTools(link);
        setState(link, u"failed"_s, reason);
    });
    client->open([this, link, client](Result<McpClient::ServerInfo> info) {
        if (!isLive(link) || link->client.get() != client) return;
        if (!info) {
            setState(link, u"failed"_s, info.error().message);
            return;
        }
        if (!info.value().tools) {
            setState(link, u"ready"_s, tr("%1 offers no tools.").arg(info.value().name));
            return;
        }
        listAndAdopt(link);
    });
}

void McpConnectors::listAndAdopt(Link *link) {
    McpClient *client = link->client.get();
    client->listTools([this, link, client](Result<QList<McpClient::Tool>> tools) {
        if (!isLive(link) || link->client.get() != client) return;
        if (!tools) {
            setState(link, u"failed"_s, tools.error().message);
            return;
        }
        adoptTools(link, tools.value());
    });
}

void McpConnectors::adoptTools(Link *link, const QList<McpClient::Tool> &tools) {
    dropTools(link);
    const QPointer<McpClient> client(link->client.get());
    const QString serverName = link->config.name.isEmpty() ? link->config.id : link->config.name;
    for (const McpClient::Tool &tool : tools) {
        ToolSpec spec;
        spec.name = toolName(link->config.id, tool.name);
        spec.description = u"[%1] %2"_s.arg(serverName, tool.description.isEmpty() ? tool.name : tool.description);
        spec.parameters = tool.inputSchema.isEmpty() ? schema::object() : tool.inputSchema;
        if (!spec.parameters.contains(u"type"_s)) spec.parameters.insert(u"type"_s, u"object"_s);
        spec.mutating = true;
        spec.privileged = true;
        spec.risk = tr("calls the %1 connector").arg(serverName);
        if (shaping_.spec) {
            std::optional<ToolSpec> shaped = shaping_.spec(tool, spec);
            if (!shaped) continue;
            spec = std::move(*shaped);
        }
        const QString remoteName = tool.name;
        const Shaping shaping = shaping_;
        registry_->add(std::make_unique<FunctionTool>(spec, [client, remoteName, shaping](const QJsonObject &arguments, ITool::Done done) {
            if (!client || !client->isReady()) {
                done(Error{ErrorCode::NotSupported, tr("The connector is not connected.")});
                return;
            }
            const QJsonObject sent = shaping.arguments ? shaping.arguments(remoteName, arguments) : arguments;
            client->callTool(remoteName, sent, [done, remoteName, shaping](Result<McpClient::CallResult> call) {
                if (!call) {
                    done(call.error());
                    return;
                }
                ToolResult result{call.value().text, std::nullopt};
                if (!call.value().images.isEmpty()) result.image = call.value().images.constFirst();
                done(shaping.result ? shaping.result(remoteName, std::move(result)) : std::move(result));
            });
        }));
        link->toolNames.append(spec.name);
    }
    link->status.tools = link->toolNames;
    qCInfo(lcMcp).noquote() << serverName << "offers" << tools.size() << "tools," << link->toolNames.size() << "taken";
    setState(link, u"ready"_s, tr("%n tool(s)", nullptr, int(link->toolNames.size())));
}

void McpConnectors::dropTools(Link *link) {
    for (const QString &name : link->toolNames) registry_->remove(name);
    link->toolNames.clear();
    link->status.tools.clear();
}

void McpConnectors::probe(const McpServerConfig &server, HttpClient *http, QObject *context,
                          std::function<void(Result<QStringList>)> done) {
    auto *client = new McpClient(makeTransport(server, http), context);
    const auto finish = [client, done](Result<QStringList> result) {
        client->close();
        client->deleteLater();
        done(std::move(result));
    };
    client->open([client, finish](Result<McpClient::ServerInfo> info) {
        if (!info) {
            finish(info.error());
            return;
        }
        if (!info.value().tools) {
            finish(QStringList{});
            return;
        }
        client->listTools([finish](Result<QList<McpClient::Tool>> tools) {
            if (!tools) {
                finish(tools.error());
                return;
            }
            QStringList names;
            for (const McpClient::Tool &tool : tools.value()) names.append(tool.name);
            finish(names);
        });
    });
}

} // namespace nexa
