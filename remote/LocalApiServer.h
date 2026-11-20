#pragma once

#include "core/ai/ProviderRegistry.h"
#include "core/ai/TurnLedger.h"
#include "core/config/Config.h"

#include <QByteArray>
#include <QHash>
#include <QObject>
#include <QTcpServer>

#include <functional>
#include <list>
#include <memory>
#include <unordered_map>

class QTcpSocket;

namespace nexa {

class HttpClient;

// M15 Part B: Loopback-only (127.0.0.1) OpenAI-compatible HTTP server exposing GET /v1/models
// and POST /v1/chat/completions over ProviderChain + an O(1) bounded LRU TurnLedger session pool,
// with zero PC-tool execution (off by default, Bearer-token protected).
class LocalApiServer : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSessions = 16;
    static constexpr qsizetype kMaxRequestBytes = 4 * 1024 * 1024;

    LocalApiServer(ProviderRegistry *registry, QObject *parent = nullptr);
    ~LocalApiServer() override;

    bool start(const Config &config, QString *error = nullptr);
    void stop();
    void updateConfig(const Config &config);
    // Optional hook (e.g. injecting runtime Nexa AI bearer tokens + entitlement checks) applied on each request
    // without touching persisted Config.
    using ConfigTransform = std::function<Result<Config>(const Config &)>;
    void setConfigTransform(ConfigTransform fn) { transform_ = std::move(fn); }

    [[nodiscard]] bool isListening() const { return server_.isListening(); }
    [[nodiscard]] quint16 port() const { return server_.serverPort(); }
    [[nodiscard]] int sessionCount() const { return int(lruOrder_.size()); }
    [[nodiscard]] bool hasSession(const QString &key) const;

    // 256-bit cryptographically seeded URL-safe bearer token (43 chars).
    [[nodiscard]] static QString generateBearerToken();

    // Pure constant-time bearer header check (unit-tested).
    [[nodiscard]] static bool matchesBearer(QByteArrayView header, const QString &expectedToken);

signals:
    void requestServed(const QString &method, const QString &path, int status);

private:
    struct SessionEntry {
        QString key;
        QString signature;          // detects config/model changes requiring a fresh chain
        std::unique_ptr<IAiProvider> chain;
        TurnLedger ledger;
        bool busy{false};
    };

    struct HttpConnection {
        QByteArray buffer;
        bool dispatched{false};
        std::shared_ptr<ChatStream> activeStream;
        QString busySessionKey;
    };

    void onNewConnection();
    void onReadyRead(QTcpSocket *socket);
    void handleHttpRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                           const QHash<QByteArray, QByteArray> &headers, const QByteArray &body);
    void handleModels(QTcpSocket *socket);
    void handleChatCompletions(QTcpSocket *socket, const QHash<QByteArray, QByteArray> &headers,
                               const QByteArray &body);

    // O(1) bounded LRU lookup/insert/touch returning pointer to slot in `lruOrder_`.
    SessionEntry *acquireSession(const QString &key, const QString &configSig, QString *error);
    void touchSession(const QString &key);

    static void sendJson(QTcpSocket *socket, int status, const QByteArray &statusText, const QByteArray &json);
    static void sendOpenAiError(QTcpSocket *socket, int status, const QString &message, const QString &type,
                                const QString &code);

    ProviderRegistry *registry_{nullptr};
    ConfigTransform transform_;
    Config config_;
    QTcpServer server_;
    QHash<QTcpSocket *, HttpConnection> connections_;

    std::list<SessionEntry> lruOrder_;
    std::unordered_map<QString, std::list<SessionEntry>::iterator> sessionIndex_;
};

} // namespace nexa
