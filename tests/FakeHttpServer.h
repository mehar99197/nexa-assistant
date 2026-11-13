#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

#include <algorithm>
#include <memory>

namespace nexa::test {

// Minimal HTTP/1.1 server on 127.0.0.1 for provider tests. Answers every request with the
// configured response, writing the body in separate delayed chunks so the client sees
// several readyRead() calls; the body ends when the connection closes.
class FakeHttpServer {
public:
    struct Response {
        int status{200};
        QByteArray contentType{"text/event-stream"};
        QList<QByteArray> chunks;
        bool holdOpen{false};       // never close: the client has to cancel
        // A whole file instead of event chunks: sent with Content-Length in three pieces; with `honourRange`,
        // a request's `Range: bytes=N-` is answered 206 with the tail (else 200 with everything).
        QByteArray body;
        bool honourRange{false};
        qint64 sendOnly{-1};        // with a body: only this many bytes are sent (the rest never comes — a stall)
    };

    FakeHttpServer() {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] { accept(); });
    }

    QUrl baseUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(server_.serverPort())); }
    void respond(Response response) { response_ = std::move(response); }

    const QByteArray &requestHead() const { return requestHead_; }
    const QByteArray &requestBody() const { return requestBody_; }
    const QList<QByteArray> &requestHeads() const { return requestHeads_; }     // every request so far

private:
    void accept() {
        while (QTcpSocket *socket = server_.nextPendingConnection()) {
            auto received = std::make_shared<QByteArray>();
            QObject::connect(socket, &QTcpSocket::readyRead, socket, [this, socket, received] {
                received->append(socket->readAll());
                const qsizetype headerEnd = received->indexOf("\r\n\r\n");
                if (headerEnd < 0) return;
                const QByteArray head = received->left(headerEnd);
                const qsizetype bodyLength = contentLength(head);
                if (received->size() < headerEnd + 4 + bodyLength) return;
                requestHead_ = canonical(head);
                requestBody_ = received->mid(headerEnd + 4, bodyLength);
                requestHeads_.append(requestHead_);
                send(socket);
            });
        }
    }

    // Header names as the tests write them ("Authorization", "Content-Length"): Qt 6.8 sends them in lower case,
    // 6.11 as given, and HTTP reads them case-insensitively anyway.
    static QByteArray canonical(const QByteArray &head) {
        QList<QByteArray> lines = head.split('\n');
        for (qsizetype i = 1; i < lines.size(); ++i) {
            const qsizetype colon = lines[i].indexOf(':');
            if (colon <= 0) continue;
            QByteArray name = lines[i].left(colon).toLower();
            bool upper = true;
            for (char &c : name) {
                if (upper && c >= 'a' && c <= 'z') c = char(c - 'a' + 'A');
                upper = (c == '-');
            }
            lines[i] = name + lines[i].mid(colon);
        }
        return lines.join('\n');
    }

    static qsizetype contentLength(const QByteArray &head) {
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray lower = line.trimmed().toLower();
            if (lower.startsWith("content-length:")) return lower.mid(15).trimmed().toLongLong();
        }
        return 0;
    }

    static qint64 rangeStart(const QByteArray &head) {
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray lower = line.trimmed().toLower();
            if (lower.startsWith("range: bytes=")) return lower.mid(13).split('-').first().toLongLong();
        }
        return -1;
    }

    void send(QTcpSocket *socket) {
        int status = response_.status;
        QList<QByteArray> chunks = response_.chunks;
        QByteArray extra;
        if (!response_.body.isEmpty()) {
            QByteArray body = response_.body;
            const qint64 start = rangeStart(requestHead_);
            if (response_.honourRange && start > 0 && start < body.size()) {
                status = 206;
                extra += "Content-Range: bytes " + QByteArray::number(start) + '-' + QByteArray::number(body.size() - 1)
                         + '/' + QByteArray::number(body.size()) + "\r\n";
                body = body.mid(start);
            }
            extra += "Content-Length: " + QByteArray::number(body.size()) + "\r\n";
            if (response_.sendOnly >= 0) body.truncate(response_.sendOnly);
            const qsizetype piece = std::max<qsizetype>(1, body.size() / 3);
            chunks.clear();
            for (qsizetype at = 0; at < body.size(); at += piece) chunks.append(body.mid(at, piece));
        }
        const QByteArray reason = status < 400 ? "OK" : "Error";
        QByteArray head = "HTTP/1.1 " + QByteArray::number(status) + ' ' + reason + "\r\n"
            + "Content-Type: " + response_.contentType + "\r\n" + extra;
        if (!response_.holdOpen) head += "Connection: close\r\n";
        socket->write(head + "\r\n");

        int delay = 0;
        for (const QByteArray &chunk : chunks) {
            delay += 5;
            QTimer::singleShot(delay, socket, [socket, chunk] { socket->write(chunk); });
        }
        if (!response_.holdOpen) {
            QTimer::singleShot(delay + 10, socket, [socket] { socket->disconnectFromHost(); });
        }
    }

    QTcpServer server_;
    Response response_;
    QByteArray requestHead_;
    QByteArray requestBody_;
    QList<QByteArray> requestHeads_;
};

} // namespace nexa::test
