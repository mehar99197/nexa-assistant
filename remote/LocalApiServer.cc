#include "remote/LocalApiServer.h"

#include "core/Logging.h"
#include "core/ai/ChatRequest.h"
#include "core/ai/ChatStream.h"
#include "core/ai/ClaudeCodeEvents.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QHostAddress>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QJsonValue>
#include <QPointer>
#include <QRandomGenerator>
#include <QTcpSocket>
#include <QTimer>
#include <QUuid>

#include <algorithm>
#include <array>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

bool constantTimeEquals(QByteArrayView a, QByteArrayView b) {
    if (a.size() != b.size()) return false;
    unsigned char diff = 0;
    for (qsizetype i = 0; i < a.size(); ++i) {
        diff |= static_cast<unsigned char>(a.at(i) ^ b.at(i));
    }
    return diff == 0;
}

QString extractMessageText(const QJsonValue &content) {
    if (content.isString()) return content.toString();
    if (!content.isArray()) return {};
    QString out;
    for (const QJsonValue &part : content.toArray()) {
        const QJsonObject obj = part.toObject();
        if (obj.value(u"type"_s).toString() == u"text"_s) out += obj.value(u"text"_s).toString();
    }
    return out;
}

Role parseRole(const QString &role) {
    if (role == u"system"_s || role == u"developer"_s) return Role::System;
    if (role == u"assistant"_s) return Role::Assistant;
    return Role::User; // "tool" or unknown mapped cleanly to user without executing any tools
}

QByteArray sseChunk(const QString &id, qint64 created, const QString &model, const QJsonObject &delta,
                    const QJsonValue &finishReason = QJsonValue::Null) {
    const QJsonObject payload{
        {u"id"_s, id},
        {u"object"_s, u"chat.completion.chunk"_s},
        {u"created"_s, created},
        {u"model"_s, model},
        {u"choices"_s, QJsonArray{QJsonObject{{u"index"_s, 0}, {u"delta"_s, delta}, {u"finish_reason"_s, finishReason}}}},
    };
    return "data: " + QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n\n";
}

QString finishReasonString(FinishReason reason) {
    switch (reason) {
    case FinishReason::Length: return u"length"_s;
    case FinishReason::ContentFilter: return u"content_filter"_s;
    default: return u"stop"_s;
    }
}

QString buildSessionKey(const QHash<QByteArray, QByteArray> &headers, const QJsonObject &body, const QString &model) {
    if (const QByteArray explicitKey = headers.value("x-nexa-session").trimmed(); !explicitKey.isEmpty()) {
        return QString::fromUtf8(explicitKey) + u'|' + model;
    }
    if (const QString user = body.value(u"user"_s).toString().trimmed(); !user.isEmpty()) {
        return u"user:"_s + user + u'|' + model;
    }
    // Derive deterministic key from conversation prefix (system + first user turn) so multi-turn
    // OpenAI clients automatically reuse the same warm ProviderChain + TurnLedger slot.
    QCryptographicHash hash(QCryptographicHash::Sha256);
    const QJsonArray messages = body.value(u"messages"_s).toArray();
    for (const QJsonValue &item : messages) {
        const QJsonObject msg = item.toObject();
        const QString role = msg.value(u"role"_s).toString();
        if (role == u"system"_s || role == u"developer"_s || role == u"user"_s) {
            hash.addData(role.toUtf8());
            hash.addData(":");
            hash.addData(extractMessageText(msg.value(u"content"_s)).toUtf8());
            hash.addData("\n");
            if (role == u"user"_s) break;
        }
    }
    hash.addData(model.toUtf8());
    return QString::fromLatin1(hash.result().toHex().left(32));
}

QString configSignature(const Config &config) {
    QStringList parts{config.activeProvider};
    for (const ProviderConfig &p : config.providers) {
        const QString keyHash = p.apiKey.isEmpty()
            ? QString()
            : QString::fromLatin1(QCryptographicHash::hash(p.apiKey.toUtf8(), QCryptographicHash::Sha256).toHex().left(12));
        parts.append(p.id + u':' + p.kind + u':' + p.baseUrl + u':' + p.defaultModel + u':'
                     + (p.fallback ? u"1"_s : u"0"_s) + u':' + keyHash);
    }
    return QString::fromLatin1(QCryptographicHash::hash(parts.join(u'|').toUtf8(), QCryptographicHash::Sha256).toHex());
}

} // namespace

QString LocalApiServer::generateBearerToken() {
    std::array<quint32, 8> words{};
    QRandomGenerator::system()->fillRange(words.data(), words.size());
    const QByteArray raw(reinterpret_cast<const char *>(words.data()), qsizetype(words.size() * sizeof(quint32)));
    return QString::fromLatin1(raw.toBase64(QByteArray::Base64UrlEncoding | QByteArray::OmitTrailingEquals));
}

bool LocalApiServer::matchesBearer(QByteArrayView header, const QString &expectedToken) {
    if (expectedToken.isEmpty()) return false;
    const QByteArray expectedUtf8 = expectedToken.toUtf8();
    const QByteArray trimmed = header.toByteArray().trimmed();
    if (trimmed.size() <= 7) return false;
    if (trimmed.left(7).toLower() != "bearer ") return false;
    return constantTimeEquals(QByteArrayView(trimmed).sliced(7).trimmed(), expectedUtf8);
}

LocalApiServer::LocalApiServer(ProviderRegistry *registry, QObject *parent)
    : QObject(parent), registry_(registry) {
    connect(&server_, &QTcpServer::newConnection, this, &LocalApiServer::onNewConnection);
}

LocalApiServer::~LocalApiServer() {
    stop();
}

bool LocalApiServer::start(const Config &config, QString *error) {
    stop();
    config_ = config;
    if (config_.localApi.bearerToken.trimmed().isEmpty()) {
        if (error) *error = tr("Local API server requires a bearer token.");
        return false;
    }
    const quint16 port = config_.localApi.port;
    // Loopback ONLY: never expose on LAN/0.0.0.0.
    if (!server_.listen(QHostAddress::LocalHost, port)) {
        if (error) *error = server_.errorString();
        return false;
    }
    qCInfo(lcApp) << "Local OpenAI-compatible API server listening on 127.0.0.1:" << server_.serverPort();
    return true;
}

void LocalApiServer::stop() {
    if (server_.isListening()) server_.close();
    const QList<QTcpSocket *> openSockets = connections_.keys();
    for (QTcpSocket *socket : openSockets) {
        std::shared_ptr<ChatStream> stream;
        if (auto it = connections_.find(socket); it != connections_.end()) {
            stream = std::move(it->activeStream);
        }
        socket->disconnect(this);
        if (stream) {
            stream->cancel();
            QTimer::singleShot(0, this, [stream = std::move(stream)]() mutable { stream.reset(); });
        }
        socket->abort();
        socket->deleteLater();
    }
    connections_.clear();
    sessionIndex_.clear();
    lruOrder_.clear();
}

void LocalApiServer::updateConfig(const Config &config) {
    const bool portChanged = config_.localApi.port != config.localApi.port;
    const bool enabledChanged = config_.localApi.enabled != config.localApi.enabled;
    config_ = config;
    if (!config_.localApi.enabled) {
        stop();
        return;
    }
    if (enabledChanged || portChanged || !server_.isListening()) {
        QString err;
        start(config_, &err);
    }
}

bool LocalApiServer::hasSession(const QString &key) const {
    return sessionIndex_.find(key) != sessionIndex_.end();
}

void LocalApiServer::onNewConnection() {
    while (QTcpSocket *socket = server_.nextPendingConnection()) {
        connections_.insert(socket, HttpConnection{});
        connect(socket, &QTcpSocket::readyRead, this, [this, socket] { onReadyRead(socket); });
        connect(socket, &QTcpSocket::disconnected, this, [this, socket] {
            if (auto it = connections_.find(socket); it != connections_.end()) {
                HttpConnection conn = std::move(it.value());
                connections_.erase(it);
                if (!conn.busySessionKey.isEmpty()) {
                    if (auto sit = sessionIndex_.find(conn.busySessionKey); sit != sessionIndex_.end()) {
                        sit->second->busy = false;
                        sit->second->ledger.reset();
                    }
                }
                if (conn.activeStream) {
                    std::shared_ptr<ChatStream> stream = std::move(conn.activeStream);
                    stream->cancel();
                    QTimer::singleShot(0, this, [stream = std::move(stream)]() mutable { stream.reset(); });
                }
            }
            socket->deleteLater();
        });
    }
}

void LocalApiServer::onReadyRead(QTcpSocket *socket) {
    auto it = connections_.find(socket);
    if (it == connections_.end() || it->dispatched) return;

    it->buffer.append(socket->readAll());
    if (it->buffer.size() > kMaxRequestBytes) {
        it->dispatched = true;
        sendOpenAiError(socket, 413, u"Request body too large."_s, u"invalid_request_error"_s, u"payload_too_large"_s);
        return;
    }

    const qsizetype hdrEnd = it->buffer.indexOf("\r\n\r\n");
    if (hdrEnd < 0) return;

    const QList<QByteArray> lines = it->buffer.left(hdrEnd).split('\n');
    if (lines.isEmpty()) {
        it->dispatched = true;
        sendOpenAiError(socket, 400, u"Malformed HTTP request."_s, u"invalid_request_error"_s, u"bad_request"_s);
        return;
    }
    const QList<QByteArray> reqLine = lines.first().trimmed().split(' ');
    if (reqLine.size() < 2) {
        it->dispatched = true;
        sendOpenAiError(socket, 400, u"Malformed HTTP request line."_s, u"invalid_request_error"_s, u"bad_request"_s);
        return;
    }
    const QByteArray method = reqLine.at(0).toUpper();
    QByteArray target = reqLine.at(1);
    if (const qsizetype q = target.indexOf('?'); q >= 0) target.truncate(q);

    QHash<QByteArray, QByteArray> headers;
    for (qsizetype i = 1; i < lines.size(); ++i) {
        const QByteArray line = lines.at(i).trimmed();
        const qsizetype colon = line.indexOf(':');
        if (colon <= 0) continue;
        headers.insert(line.left(colon).trimmed().toLower(), line.mid(colon + 1).trimmed());
    }

    bool okLen = true;
    const qsizetype contentLength = headers.contains("content-length")
        ? headers.value("content-length").toLongLong(&okLen) : 0;
    if (!okLen || contentLength < 0 || contentLength > kMaxRequestBytes) {
        it->dispatched = true;
        sendOpenAiError(socket, 400, u"Invalid Content-Length."_s, u"invalid_request_error"_s, u"bad_request"_s);
        return;
    }
    const qsizetype bodyStart = hdrEnd + 4;
    if (it->buffer.size() - bodyStart < contentLength) return; // wait for complete body

    it->dispatched = true;
    const QByteArray body = it->buffer.mid(bodyStart, contentLength);
    it->buffer.clear();
    handleHttpRequest(socket, method, target, headers, body);
}

void LocalApiServer::handleHttpRequest(QTcpSocket *socket, const QByteArray &method, const QByteArray &target,
                                       const QHash<QByteArray, QByteArray> &headers, const QByteArray &body) {
    if (!matchesBearer(headers.value("authorization"), config_.localApi.bearerToken)) {
        emit requestServed(QString::fromLatin1(method), QString::fromUtf8(target), 401);
        sendOpenAiError(socket, 401, u"Invalid or missing bearer token."_s, u"invalid_request_error"_s,
                        u"unauthorized"_s);
        return;
    }

    if (method == "GET" && target == "/v1/models") {
        handleModels(socket);
        return;
    }
    if (method == "POST" && target == "/v1/chat/completions") {
        handleChatCompletions(socket, headers, body);
        return;
    }
    emit requestServed(QString::fromLatin1(method), QString::fromUtf8(target), 404);
    sendOpenAiError(socket, 404, u"Unknown endpoint."_s, u"invalid_request_error"_s, u"not_found"_s);
}

void LocalApiServer::handleModels(QTcpSocket *socket) {
    auto respond = [this, socket](const QStringList &ids) {
        QJsonArray data;
        for (const QString &id : ids) {
            data.append(QJsonObject{{u"id"_s, id}, {u"object"_s, u"model"_s}, {u"created"_s, 0}, {u"owned_by"_s, u"nexa"_s}});
        }
        emit requestServed(u"GET"_s, u"/v1/models"_s, 200);
        sendJson(socket, 200, "OK", QJsonDocument(QJsonObject{{u"object"_s, u"list"_s}, {u"data"_s, data}}).toJson(QJsonDocument::Compact));
    };

    QStringList fallbackModels;
    for (const ProviderConfig &p : config_.providers) {
        if (!p.defaultModel.trimmed().isEmpty()) fallbackModels.append(p.defaultModel.trimmed());
    }
    if (fallbackModels.isEmpty()) fallbackModels.append(u"default"_s);
    fallbackModels.removeDuplicates();

    Config effective = config_;
    if (transform_) {
        if (Result<Config> transformed = transform_(effective)) effective = std::move(transformed).value();
    }

    if (!registry_) {
        respond(fallbackModels);
        return;
    }
    const ProviderConfig *active = effective.active();
    if (!active) {
        respond(fallbackModels);
        return;
    }
    Result<std::unique_ptr<IAiProvider>> created = registry_->create(*active);
    if (!created) {
        respond(fallbackModels);
        return;
    }
    auto provider = std::shared_ptr<IAiProvider>(std::move(created).value().release(), [](IAiProvider *p) {
        if (p) p->deleteLater();
    });
    const QPointer<LocalApiServer> self(this);
    const QPointer<QTcpSocket> sockPtr(socket);
    provider->listModels([self, respond, fallbackModels, provider, sockPtr](Result<QStringList> models) {
        if (!self || !sockPtr || sockPtr->state() != QAbstractSocket::ConnectedState) return;
        if (models && !models.value().isEmpty()) {
            respond(models.value());
        } else {
            respond(fallbackModels);
        }
    });
}

void LocalApiServer::touchSession(const QString &key) {
    const auto it = sessionIndex_.find(key);
    if (it == sessionIndex_.end()) return;
    lruOrder_.splice(lruOrder_.begin(), lruOrder_, it->second);
}

LocalApiServer::SessionEntry *LocalApiServer::acquireSession(const QString &key, const QString &configSig,
                                                             QString *error) {
    Config effective = config_;
    if (transform_) {
        Result<Config> transformed = transform_(effective);
        if (!transformed) {
            if (error) *error = transformed.error().message;
            return nullptr;
        }
        effective = std::move(transformed).value();
    }
    const QString effectiveSig = configSig + u'|' + configSignature(effective);

    if (const auto it = sessionIndex_.find(key); it != sessionIndex_.end()) {
        lruOrder_.splice(lruOrder_.begin(), lruOrder_, it->second);
        SessionEntry &entry = *it->second;
        if (entry.busy) {
            if (error) *error = u"A request for this session is already running."_s;
            return nullptr;
        }
        if (entry.signature != effectiveSig) {
            Result<std::unique_ptr<IAiProvider>> rebuilt = registry_->createChain(effective);
            if (!rebuilt) {
                if (error) *error = rebuilt.error().message;
                return nullptr;
            }
            entry.chain = std::move(rebuilt).value();
            entry.ledger.reset();
            entry.signature = effectiveSig;
        }
        return &entry;
    }

    if (!registry_) {
        if (error) *error = u"No provider registry."_s;
        return nullptr;
    }
    Result<std::unique_ptr<IAiProvider>> chain = registry_->createChain(effective);
    if (!chain) {
        if (error) *error = chain.error().message;
        return nullptr;
    }

    // Evict least-recently-used non-busy slot when at capacity.
    while (int(lruOrder_.size()) >= kMaxSessions) {
        auto victim = std::prev(lruOrder_.end());
        while (victim != lruOrder_.begin() && victim->busy) --victim;
        if (victim->busy) {
            if (error) *error = u"All local API sessions are currently busy."_s;
            return nullptr;
        }
        sessionIndex_.erase(victim->key);
        lruOrder_.erase(victim);
    }

    SessionEntry fresh;
    fresh.key = key;
    fresh.signature = effectiveSig;
    fresh.chain = std::move(chain).value();
    lruOrder_.push_front(std::move(fresh));
    sessionIndex_.emplace(key, lruOrder_.begin());
    return &lruOrder_.front();
}

void LocalApiServer::handleChatCompletions(QTcpSocket *socket, const QHash<QByteArray, QByteArray> &headers,
                                           const QByteArray &body) {
    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(body, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        emit requestServed(u"POST"_s, u"/v1/chat/completions"_s, 400);
        sendOpenAiError(socket, 400, u"Body must be a JSON object."_s, u"invalid_request_error"_s, u"bad_request"_s);
        return;
    }
    const QJsonObject obj = doc.object();
    const QJsonArray rawMessages = obj.value(u"messages"_s).toArray();
    if (rawMessages.isEmpty()) {
        emit requestServed(u"POST"_s, u"/v1/chat/completions"_s, 400);
        sendOpenAiError(socket, 400, u"messages must be a non-empty array."_s, u"invalid_request_error"_s,
                        u"bad_request"_s);
        return;
    }

    const QString requestedModel = obj.value(u"model"_s).toString().trimmed();
    const bool streamMode = obj.value(u"stream"_s).toBool(false);

    ChatRequest request;
    request.model = requestedModel == u"default"_s ? QString() : requestedModel;
    if (obj.contains(u"temperature"_s) && obj.value(u"temperature"_s).isDouble()) {
        request.temperature = obj.value(u"temperature"_s).toDouble(-1.0);
    }
    if (obj.contains(u"max_tokens"_s) && obj.value(u"max_tokens"_s).isDouble()) {
        request.maxTokens = std::max(0, obj.value(u"max_tokens"_s).toInt(0));
    }
    for (const QJsonValue &val : rawMessages) {
        const QJsonObject msgObj = val.toObject();
        const QString text = extractMessageText(msgObj.value(u"content"_s));
        request.messages.append(ChatMessage{
            .role = parseRole(msgObj.value(u"role"_s).toString()),
            .text = text,
        });
    }
    // Security invariant: M15 Part B local OpenAI proxy NEVER exposes or runs PC tools.
    request.tools.clear();

    const QString sessionKey = buildSessionKey(headers, obj, request.model);
    const QString cfgSig = configSignature(config_);
    QString err;
    SessionEntry *slot = acquireSession(sessionKey, cfgSig, &err);
    if (!slot) {
        const int code = err.contains(u"busy"_s, Qt::CaseInsensitive)
            || err.contains(u"already running"_s, Qt::CaseInsensitive) ? 429 : 503;
        emit requestServed(u"POST"_s, u"/v1/chat/completions"_s, code);
        sendOpenAiError(socket, code, err, u"server_error"_s, code == 429 ? u"busy"_s : u"no_provider"_s);
        return;
    }

    const TurnPlan turnPlan = slot->ledger.plan(request);
    if (turnPlan.fresh && !slot->ledger.isEmpty()) {
        Config effective = config_;
        if (transform_) {
            if (Result<Config> transformed = transform_(effective)) effective = std::move(transformed).value();
        }
        if (Result<std::unique_ptr<IAiProvider>> freshChain = registry_->createChain(effective); freshChain) {
            slot->chain = std::move(freshChain).value();
        }
        slot->ledger.reset();
    }

    slot->busy = true;
    std::shared_ptr<ChatStream> stream = slot->chain->chat(request);
    if (auto connIt = connections_.find(socket); connIt != connections_.end()) {
        connIt->activeStream = stream;
        connIt->busySessionKey = sessionKey;
    }

    const QString completionId = u"chatcmpl-"_s + QUuid::createUuid().toString(QUuid::Id128).left(24);
    const qint64 createdAt = QDateTime::currentSecsSinceEpoch();
    const QString modelLabel = request.model.isEmpty()
        ? (config_.active() && !config_.active()->defaultModel.isEmpty() ? config_.active()->defaultModel : u"nexa"_s)
        : request.model;

    const QPointer<QTcpSocket> sockPtr(socket);
    const QPointer<ChatStream> streamPtr(stream.get());
    auto headersSent = std::make_shared<bool>(false);
    auto settled = std::make_shared<bool>(false);

    const auto releaseSlot = [this, sessionKey, sockPtr, settled](bool commitTurn, const ChatRequest &req) {
        if (*settled) return;
        *settled = true;
        if (const auto sit = sessionIndex_.find(sessionKey); sit != sessionIndex_.end()) {
            sit->second->busy = false;
            if (commitTurn) sit->second->ledger.commit(req);
            else sit->second->ledger.reset();
        }
        if (sockPtr) {
            if (const auto cit = connections_.find(sockPtr.data()); cit != connections_.end()) {
                cit->busySessionKey.clear();
                std::shared_ptr<ChatStream> doomed = std::move(cit->activeStream);
                if (doomed) {
                    QTimer::singleShot(0, this, [doomed = std::move(doomed)]() mutable { doomed.reset(); });
                }
            }
        }
    };

    if (streamMode) {
        connect(stream.get(), &ChatStream::textDelta, this,
                [sockPtr, headersSent, completionId, createdAt, modelLabel](const QString &delta) {
                    if (!sockPtr || sockPtr->state() != QAbstractSocket::ConnectedState || delta.isEmpty()) return;
                    if (!*headersSent) {
                        *headersSent = true;
                        sockPtr->write(
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream; charset=utf-8\r\n"
                            "Cache-Control: no-store\r\n"
                            "Connection: close\r\n\r\n");
                        sockPtr->write(sseChunk(completionId, createdAt, modelLabel,
                                                QJsonObject{{u"role"_s, u"assistant"_s}}));
                    }
                    sockPtr->write(sseChunk(completionId, createdAt, modelLabel, QJsonObject{{u"content"_s, delta}}));
                    sockPtr->flush();
                });
    }

    connect(stream.get(), &ChatStream::finished, this,
            [this, streamPtr, sockPtr, headersSent, streamMode, completionId, createdAt, modelLabel, request,
             releaseSlot](FinishReason reason) {
                const QString replyText = streamPtr ? streamPtr->text() : QString();
                releaseSlot(true, request);
                if (!sockPtr || sockPtr->state() != QAbstractSocket::ConnectedState) return;
                emit requestServed(u"POST"_s, u"/v1/chat/completions"_s, 200);
                if (streamMode) {
                    if (!*headersSent) {
                        *headersSent = true;
                        sockPtr->write(
                            "HTTP/1.1 200 OK\r\n"
                            "Content-Type: text/event-stream; charset=utf-8\r\n"
                            "Cache-Control: no-store\r\n"
                            "Connection: close\r\n\r\n");
                        sockPtr->write(sseChunk(completionId, createdAt, modelLabel,
                                                QJsonObject{{u"role"_s, u"assistant"_s}, {u"content"_s, replyText}}));
                    }
                    sockPtr->write(sseChunk(completionId, createdAt, modelLabel, QJsonObject{},
                                            finishReasonString(reason)));
                    sockPtr->write("data: [DONE]\n\n");
                    sockPtr->flush();
                    sockPtr->disconnectFromHost();
                    return;
                }
                const QJsonObject reply{
                    {u"id"_s, completionId},
                    {u"object"_s, u"chat.completion"_s},
                    {u"created"_s, createdAt},
                    {u"model"_s, modelLabel},
                    {u"choices"_s, QJsonArray{QJsonObject{
                         {u"index"_s, 0},
                         {u"message"_s, QJsonObject{{u"role"_s, u"assistant"_s}, {u"content"_s, replyText}}},
                         {u"finish_reason"_s, finishReasonString(reason)},
                     }}},
                };
                sendJson(sockPtr, 200, "OK", QJsonDocument(reply).toJson(QJsonDocument::Compact));
            });

    connect(stream.get(), &ChatStream::failed, this,
            [this, sockPtr, headersSent, streamMode, request, releaseSlot](const Error &error) {
                releaseSlot(false, request);
                if (!sockPtr || sockPtr->state() != QAbstractSocket::ConnectedState) return;
                if (streamMode && *headersSent) {
                    const QJsonObject payload{{u"error"_s, QJsonObject{{u"message"_s, error.message},
                                                                       {u"type"_s, u"upstream_error"_s}}}};
                    sockPtr->write("data: " + QJsonDocument(payload).toJson(QJsonDocument::Compact) + "\n\ndata: [DONE]\n\n");
                    sockPtr->flush();
                    sockPtr->disconnectFromHost();
                    return;
                }
                const int status = error.code == ErrorCode::Unauthorized ? 401
                    : error.code == ErrorCode::Timeout ? 504 : 502;
                emit requestServed(u"POST"_s, u"/v1/chat/completions"_s, status);
                sendOpenAiError(sockPtr, status, error.message, u"upstream_error"_s, u"provider_error"_s);
            });
}

void LocalApiServer::sendJson(QTcpSocket *socket, int status, const QByteArray &statusText, const QByteArray &json) {
    const QByteArray header = "HTTP/1.1 " + QByteArray::number(status) + ' ' + statusText
        + "\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: "
        + QByteArray::number(json.size()) + "\r\nConnection: close\r\n\r\n";
    socket->write(header);
    socket->write(json);
    socket->flush();
    socket->disconnectFromHost();
}

void LocalApiServer::sendOpenAiError(QTcpSocket *socket, int status, const QString &message, const QString &type,
                                     const QString &code) {
    const QJsonObject err{
        {u"error"_s, QJsonObject{{u"message"_s, message}, {u"type"_s, type}, {u"code"_s, code}}},
    };
    const QByteArray statusText = status == 401 ? "Unauthorized"
        : status == 400 ? "Bad Request"
        : status == 404 ? "Not Found"
        : status == 413 ? "Payload Too Large"
        : status == 429 ? "Too Many Requests"
        : status == 503 ? "Service Unavailable"
        : status == 504 ? "Gateway Timeout"
        : "Bad Gateway";
    sendJson(socket, status, statusText, QJsonDocument(err).toJson(QJsonDocument::Compact));
}

} // namespace nexa
