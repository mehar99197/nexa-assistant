#include "core/net/HttpClient.h"

#include "core/Logging.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonValue>
#include <QNetworkReply>
#include <QPointer>

#include <chrono>

using namespace Qt::StringLiterals;

namespace nexa {

HttpClient::HttpClient(QObject *parent) : QObject(parent) {}

QNetworkRequest HttpClient::jsonRequest(const QUrl &url, const QString &bearerToken) {
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setRawHeader("Accept", "application/json");
    if (!bearerToken.isEmpty()) request.setRawHeader("Authorization", "Bearer " + bearerToken.toUtf8());
    return request;
}

QNetworkReply *HttpClient::get(const QNetworkRequest &request, int timeoutMs, QObject *context,
                               BodyCallback done) {
    QNetworkRequest timed(request);
    timed.setTransferTimeout(std::chrono::milliseconds(timeoutMs));
    return deliver(manager_.get(timed), context, std::move(done));
}

QNetworkReply *HttpClient::deleteResource(const QNetworkRequest &request, int timeoutMs, QObject *context,
                                          BodyCallback done) {
    QNetworkRequest timed(request);
    timed.setTransferTimeout(std::chrono::milliseconds(timeoutMs));
    return deliver(manager_.deleteResource(timed), context, std::move(done));
}

QNetworkReply *HttpClient::post(const QNetworkRequest &request, const QByteArray &body, int timeoutMs,
                                QObject *context, BodyCallback done) {
    QNetworkRequest timed(request);
    timed.setTransferTimeout(std::chrono::milliseconds(timeoutMs));
    return deliver(manager_.post(timed, body), context, std::move(done));
}

QNetworkReply *HttpClient::postStreaming(const QNetworkRequest &request, const QByteArray &body,
                                         int idleTimeoutMs) {
    QNetworkRequest streaming(request);
    streaming.setRawHeader("Accept", "text/event-stream");
    streaming.setTransferTimeout(std::chrono::milliseconds(idleTimeoutMs));
    QNetworkReply *reply = manager_.post(streaming, body);
    watchNexaHeaders(reply);
    return reply;
}

QNetworkReply *HttpClient::getStreaming(const QNetworkRequest &request, int idleTimeoutMs) {
    QNetworkRequest streaming(request);
    streaming.setRawHeader("Accept", "text/event-stream");
    streaming.setTransferTimeout(std::chrono::milliseconds(idleTimeoutMs));
    return manager_.get(streaming);
}

void HttpClient::watchNexaHeaders(QNetworkReply *reply) {
    if (!reply) return;
    const QPointer<HttpClient> self(this);
    const auto inspect = [self, reply] {
        if (!self || !reply->hasRawHeader("X-Nexa-Remaining")) return;
        bool ok = false;
        const int remaining = reply->rawHeader("X-Nexa-Remaining").trimmed().toInt(&ok);
        if (ok && remaining >= 0) emit self->nexaRemaining(remaining);
    };
    connect(reply, &QNetworkReply::metaDataChanged, this, inspect);
    connect(reply, &QNetworkReply::finished, this, inspect);
}

QNetworkReply *HttpClient::deliver(QNetworkReply *reply, QObject *context, BodyCallback done) {
    const QPointer<HttpClient> self(this);
    const QPointer<QObject> guard(context);
    const bool guarded = context != nullptr;
    connect(reply, &QNetworkReply::finished, reply, [self, reply, guard, guarded, done = std::move(done)] {
        reply->deleteLater();
        if (self && reply->hasRawHeader("X-Nexa-Remaining")) {
            bool ok = false;
            const int remaining = reply->rawHeader("X-Nexa-Remaining").trimmed().toInt(&ok);
            if (ok && remaining >= 0) emit self->nexaRemaining(remaining);
        }
        if (guarded && !guard) return;
        const QByteArray body = reply->readAll();
        if (reply->error() != QNetworkReply::NoError || httpStatus(reply) >= 400) {
            const Error error = errorFor(reply, body);
            qCDebug(lcNet) << reply->url().toDisplayString(QUrl::RemoveQuery) << "failed:" << error.message;
            done(error);
            return;
        }
        done(body);
    });
    return reply;
}

int HttpClient::httpStatus(const QNetworkReply *reply) {
    return reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
}

Error HttpClient::errorFor(const QNetworkReply *reply, const QByteArray &body) {
    const int status = httpStatus(reply);
    if (status >= 400) {
        const ErrorCode code = (status == 401 || status == 403) ? ErrorCode::Unauthorized : ErrorCode::Http;
        QString message = u"HTTP %1"_s.arg(status);
        const QString detail = serverMessage(body);
        if (!detail.isEmpty()) message += u": "_s + detail;
        return Error{code, message};
    }
    switch (reply->error()) {
    case QNetworkReply::NoError:
        return Error{ErrorCode::Unknown, u"Unexpected response"_s};
    case QNetworkReply::ConnectionRefusedError:
        return Error{ErrorCode::ConnectionRefused, u"Connection refused by %1"_s.arg(reply->url().authority())};
    case QNetworkReply::HostNotFoundError:
        return Error{ErrorCode::Network, u"Host not found: %1"_s.arg(reply->url().host())};
    case QNetworkReply::TimeoutError:
        return Error{ErrorCode::Timeout, u"The server did not respond in time"_s};
    case QNetworkReply::OperationCanceledError:
        return Error{ErrorCode::Cancelled, u"Request cancelled or timed out"_s};
    default:
        return Error{ErrorCode::Network, reply->errorString()};
    }
}

QString HttpClient::serverMessage(const QByteArray &body) {
    const QJsonDocument doc = QJsonDocument::fromJson(body);
    if (doc.isNull()) return QString::fromUtf8(body.left(300)).simplified();

    // Gemini wraps errors in a one-element array.
    const QJsonObject root = doc.isArray() ? doc.array().first().toObject() : doc.object();
    const QJsonValue error = root.value(u"error"_s);
    if (error.isObject()) {
        const QString message = error.toObject().value(u"message"_s).toString();
        if (!message.isEmpty()) return message;
    }
    if (error.isString()) return error.toString();
    for (const QString &key : {u"message"_s, u"detail"_s}) {
        if (root.value(key).isString()) return root.value(key).toString();
    }
    return {};
}

} // namespace nexa
