#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QObject>

#include <functional>

class QNetworkReply;

namespace nexa {

// Facade over one shared QNetworkAccessManager (connection reuse, HTTP/2 where offered).
// Lives on the GUI thread; replies are read incrementally, so no call ever blocks.
class HttpClient : public QObject {
    Q_OBJECT
public:
    using BodyCallback = std::function<void(Result<QByteArray>)>;

    explicit HttpClient(QObject *parent = nullptr);

    static QNetworkRequest jsonRequest(const QUrl &url, const QString &bearerToken = {});

    // Buffered requests. `done` runs exactly once, unless `context` is destroyed first.
    // Aborting the returned reply completes it with ErrorCode::Cancelled.
    QNetworkReply *get(const QNetworkRequest &request, int timeoutMs, QObject *context, BodyCallback done);
    QNetworkReply *post(const QNetworkRequest &request, const QByteArray &body, int timeoutMs,
                        QObject *context, BodyCallback done);
    QNetworkReply *deleteResource(const QNetworkRequest &request, int timeoutMs, QObject *context, BodyCallback done);

    // Streaming POST: the caller owns the reply and reads it on readyRead(). The transfer is
    // aborted only after `idleTimeoutMs` without receiving any bytes.
    QNetworkReply *postStreaming(const QNetworkRequest &request, const QByteArray &body, int idleTimeoutMs);
    // Streaming GET, the same way: an event stream the caller reads (and aborts) itself.
    QNetworkReply *getStreaming(const QNetworkRequest &request, int idleTimeoutMs);

    static int httpStatus(const QNetworkReply *reply);
    // Error for a failed reply: HTTP status plus the server's own message, or the transport error.
    static Error errorFor(const QNetworkReply *reply, const QByteArray &body);
    // Best-effort human message from an error body ({"error":{"message":…}}, {"error":"…"}, …).
    static QString serverMessage(const QByteArray &body);

signals:
    void nexaRemaining(int remaining);

private:
    QNetworkReply *deliver(QNetworkReply *reply, QObject *context, BodyCallback done);
    void watchNexaHeaders(QNetworkReply *reply);

    QNetworkAccessManager manager_;
};

} // namespace nexa
