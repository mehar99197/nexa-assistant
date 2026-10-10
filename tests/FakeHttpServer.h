#pragma once

#include <QByteArray>
#include <QHostAddress>
#include <QList>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>

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
    };

    FakeHttpServer() {
        server_.listen(QHostAddress::LocalHost, 0);
        QObject::connect(&server_, &QTcpServer::newConnection, &server_, [this] { accept(); });
    }

    QUrl baseUrl() const { return QUrl(QStringLiteral("http://127.0.0.1:%1/v1").arg(server_.serverPort())); }
    void respond(Response response) { response_ = std::move(response); }

    const QByteArray &requestHead() const { return requestHead_; }
    const QByteArray &requestBody() const { return requestBody_; }

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
                requestHead_ = head;
                requestBody_ = received->mid(headerEnd + 4, bodyLength);
                send(socket);
            });
        }
    }

    static qsizetype contentLength(const QByteArray &head) {
        for (const QByteArray &line : head.split('\n')) {
            const QByteArray lower = line.trimmed().toLower();
            if (lower.startsWith("content-length:")) return lower.mid(15).trimmed().toLongLong();
        }
        return 0;
    }

    void send(QTcpSocket *socket) {
        const QByteArray reason = response_.status < 400 ? "OK" : "Error";
        QByteArray head = "HTTP/1.1 " + QByteArray::number(response_.status) + ' ' + reason + "\r\n"
            + "Content-Type: " + response_.contentType + "\r\n";
        if (!response_.holdOpen) head += "Connection: close\r\n";
        socket->write(head + "\r\n");

        int delay = 0;
        for (const QByteArray &chunk : response_.chunks) {
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
};

} // namespace nexa::test
