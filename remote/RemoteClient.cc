#include "remote/RemoteClient.h"

#include "core/Logging.h"

#include <QCryptographicHash>
#include <QHostAddress>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QSslConfiguration>
#include <QSslError>
#include <QSslSocket>
#include <QUrl>
#include <QWebSocket>

#include <algorithm>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kProbeTimeoutMs = 10'000;
constexpr int kRequestTimeoutMs = 15'000;

bool isLoopback(const QString &host) {
    return host.compare(u"localhost"_s, Qt::CaseInsensitive) == 0 || QHostAddress(host).isLoopback();
}

} // namespace

// ------------------------------------------------------------------ RemoteEndpoint

QString RemoteEndpoint::url(const QString &path) const {
    QUrl url;
    url.setScheme(tls ? u"https"_s : u"http"_s);
    url.setHost(host);
    url.setPort(port);
    url.setPath(path.isEmpty() ? u"/"_s : path);
    return url.toString();
}

QString RemoteEndpoint::socketUrl() const {
    QUrl url(this->url(u"/ws"_s));
    url.setScheme(tls ? u"wss"_s : u"ws"_s);
    return url.toString();
}

std::optional<RemoteEndpoint> RemoteEndpoint::parse(const QString &text) {
    const QString trimmed = text.trimmed();
    if (trimmed.isEmpty()) return std::nullopt;
    // QUrl does the host:port reading, IPv6 brackets included.
    const QUrl url(trimmed.contains(u"://"_s) ? trimmed : u"https://"_s + trimmed);
    if (!url.isValid() || url.host().isEmpty()) return std::nullopt;
    RemoteEndpoint endpoint;
    endpoint.host = url.host();
    endpoint.port = url.port(kDefaultPort);
    endpoint.tls = !isLoopback(endpoint.host);
    static const QRegularExpression invitation(u"pair=(\\d{6})"_s);
    endpoint.code = invitation.match(url.fragment()).captured(1);
    return endpoint;
}

QString fingerprintOf(const QSslCertificate &certificate) {
    return QString::fromLatin1(certificate.digest(QCryptographicHash::Sha256).toHex(':')).toUpper();
}

// ------------------------------------------------------------------ RemoteClient

RemoteClient::RemoteClient(QObject *parent)
    : QObject(parent), socket_(std::make_unique<QWebSocket>()), network_(std::make_unique<QNetworkAccessManager>()) {
    connect(socket_.get(), &QWebSocket::connected, this, &RemoteClient::onConnected);
    connect(socket_.get(), &QWebSocket::stateChanged, this, &RemoteClient::onSocketStateChanged);
    connect(socket_.get(), &QWebSocket::textMessageReceived, this, &RemoteClient::onTextMessage);
    connect(socket_.get(), &QWebSocket::sslErrors, this, &RemoteClient::onSocketSslErrors);

    // Keeps the connection alive through Wi-Fi power saving and idle-closing routers.
    ping_.setInterval(kPingIntervalMs);
    connect(&ping_, &QTimer::timeout, this, [this] {
        if (welcomed_) socket_->sendTextMessage(QString::fromUtf8(remote::ping()));
    });
    retry_.setSingleShot(true);
    connect(&retry_, &QTimer::timeout, this, &RemoteClient::open);
}

RemoteClient::~RemoteClient() {
    wanted_ = false;
    socket_->abort();
}

void RemoteClient::setEndpoint(const RemoteEndpoint &endpoint) {
    endpoint_ = endpoint;
    rejected_ = false;
}

void RemoteClient::probe(std::function<void(Result<Certificate>)> done) {
    if (!endpoint_.tls) {
        done(Certificate{{}, true});
        return;
    }
    auto *socket = new QSslSocket(this);
    auto reported = std::make_shared<bool>(false);
    // The handshake is still on the stack when sslErrors arrives, so neither the socket's end nor
    // the caller's next step (which may run a dialog's event loop) can happen inside it.
    const auto report = [this, socket, done, reported](Result<Certificate> result) {
        if (*reported) return;
        *reported = true;
        QMetaObject::invokeMethod(this, [socket, done, result = std::move(result)] {
            socket->abort();
            socket->deleteLater();
            done(result);
        }, Qt::QueuedConnection);
    };
    connect(socket, &QSslSocket::sslErrors, this, [socket, report](const QList<QSslError> &errors) {
        QSslCertificate certificate = socket->peerCertificate();
        for (const QSslError &error : errors) {
            if (certificate.isNull()) certificate = error.certificate();
        }
        if (certificate.isNull()) {
            report(Error{ErrorCode::Network, u"Nexa did not show a certificate."_s});
            return;
        }
        report(Certificate{fingerprintOf(certificate), false});
    });
    connect(socket, &QSslSocket::encrypted, this, [socket, report] {
        report(Certificate{fingerprintOf(socket->peerCertificate()), true});
    });
    connect(socket, &QSslSocket::errorOccurred, this, [socket, report](QAbstractSocket::SocketError) {
        report(Error{ErrorCode::Network, socket->errorString()});
    });
    QTimer::singleShot(kProbeTimeoutMs, socket, [report] {
        report(Error{ErrorCode::Timeout, u"Nexa did not answer."_s});
    });
    socket->connectToHostEncrypted(endpoint_.host, quint16(endpoint_.port));
}

void RemoteClient::pair(const QString &code, const QString &deviceName, std::function<void(Result<QString>)> done) {
    QNetworkRequest request{QUrl(endpoint_.url(u"/api/pair"_s))};
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setTransferTimeout(kRequestTimeoutMs);
    const QByteArray body = QJsonDocument(QJsonObject{{u"code"_s, code}, {u"name"_s, deviceName}}).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = network_->post(request, body);
    guard(reply);
    connect(reply, &QNetworkReply::finished, this, [this, reply, done = std::move(done)] {
        reply->deleteLater();
        if (reply->error() == QNetworkReply::SslHandshakeFailedError) {
            done(Error{ErrorCode::PermissionDenied, u"Nexa's certificate is not the one that was trusted."_s});
            return;
        }
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QJsonObject answer = QJsonDocument::fromJson(reply->readAll()).object();
        const QString token = answer.value(u"token"_s).toString();
        if (status == 200 && !token.isEmpty()) {
            token_ = token;
            done(token);
            return;
        }
        QString message = answer.value(u"error"_s).toString();
        if (message.isEmpty()) message = status > 0 ? u"Pairing failed (%1)."_s.arg(status) : reply->errorString();
        done(Error{status == 403 ? ErrorCode::PermissionDenied : ErrorCode::Network, message});
    });
}

void RemoteClient::connectToNexa() {
    wanted_ = true;
    rejected_ = false;
    attempts_ = 0;
    retry_.stop();
    open();
}

void RemoteClient::disconnectFromNexa() {
    wanted_ = false;
    retry_.stop();
    ping_.stop();
    welcomed_ = false;
    if (socket_->state() != QAbstractSocket::UnconnectedState) socket_->close();
    setState(State::Offline);
}

void RemoteClient::open() {
    if (!wanted_ || socket_->state() != QAbstractSocket::UnconnectedState) return;
    if (!endpoint_.isValid() || token_.isEmpty()) {
        wanted_ = false;
        setState(State::Offline, u"Not paired."_s);
        return;
    }
    if (endpoint_.tls && pinned_.isEmpty()) {
        wanted_ = false;
        setState(State::Offline, u"Nexa's certificate has not been trusted yet."_s);
        return;
    }
    welcomed_ = false;
    setState(State::Connecting, u"connecting…"_s);
    socket_->open(QUrl(endpoint_.socketUrl()));
}

void RemoteClient::onConnected() {
    socket_->sendTextMessage(QString::fromUtf8(remote::hello(token_)));
}

void RemoteClient::onSocketStateChanged() {
    if (socket_->state() != QAbstractSocket::UnconnectedState) return;
    ping_.stop();
    welcomed_ = false;
    if (!wanted_) {
        setState(State::Offline);
        return;
    }
    if (rejected_) {
        wanted_ = false;
        setState(State::Offline, u"Nexa's certificate is not the one that was trusted."_s);
        return;
    }
    if (retry_.isActive()) return;
    const int delay = std::min(kMaxRetryDelayMs, 1000 * (1 << std::min(attempts_, 4)));
    ++attempts_;
    retry_.start(delay);
    setState(State::Waiting, u"offline — trying again in %1 s"_s.arg(delay / 1000));
}

void RemoteClient::onTextMessage(const QString &text) {
    const std::optional<remote::ServerMessage> message = remote::decodeServer(text.toUtf8());
    if (!message) {
        qCWarning(lcNet).noquote() << "remote client: unreadable message from Nexa";
        return;
    }
    switch (message->type) {
    case remote::ServerMessage::Type::Welcome:
        welcomed_ = true;
        attempts_ = 0;
        ping_.start();
        setState(State::Online, message->device);
        emit welcomed(message->device, message->voice, message->busy);
        return;
    case remote::ServerMessage::Type::Pong:
        return;
    case remote::ServerMessage::Type::Error:
        if (message->code == u"unauthorized") {
            // The phone was unpaired on the PC. Nexa closes the socket next; no retrying with this token.
            token_.clear();
            wanted_ = false;
            emit unauthorized(message->text);
            return;
        }
        break;
    default:
        break;
    }
    emit received(*message);
}

void RemoteClient::onSocketSslErrors(const QList<QSslError> &errors) {
    QString seen;
    if (trusts(errors, socket_->sslConfiguration().peerCertificate(), &seen)) {
        socket_->ignoreSslErrors();
        return;
    }
    rejected_ = true;
    qCWarning(lcNet).noquote() << "remote client: refused a certificate that is not the trusted one";
    emit certificateRejected(seen);
}

void RemoteClient::guard(QNetworkReply *reply) {
    connect(reply, &QNetworkReply::sslErrors, this, [this, reply](const QList<QSslError> &errors) {
        QString seen;
        if (trusts(errors, reply->sslConfiguration().peerCertificate(), &seen)) {
            reply->ignoreSslErrors();
            return;
        }
        emit certificateRejected(seen);
    });
}

// Every complaint must be about the certificate the user pinned, and nothing else is ignored.
bool RemoteClient::trusts(const QList<QSslError> &errors, const QSslCertificate &peer, QString *seen) const {
    if (!peer.isNull() && seen) *seen = fingerprintOf(peer);
    if (pinned_.isEmpty()) return false;
    for (const QSslError &error : errors) {
        const QSslCertificate certificate = error.certificate().isNull() ? peer : error.certificate();
        if (certificate.isNull()) return false;
        const QString fingerprint = fingerprintOf(certificate);
        if (seen) *seen = fingerprint;
        if (fingerprint != pinned_) return false;
    }
    return true;
}

bool RemoteClient::ask(const QString &text) {
    if (!welcomed_) return false;
    socket_->sendTextMessage(QString::fromUtf8(remote::text(text)));
    return true;
}

bool RemoteClient::askAloud(const QByteArray &pcm16kMonoInt16) {
    if (!welcomed_ || pcm16kMonoInt16.isEmpty() || pcm16kMonoInt16.size() > remote::kMaxAudioFrame) return false;
    socket_->sendBinaryMessage(pcm16kMonoInt16);
    return true;
}

void RemoteClient::cancel() {
    if (welcomed_) socket_->sendTextMessage(QString::fromUtf8(remote::cancel()));
}

void RemoteClient::fetch(const QString &path, std::function<void(Result<QByteArray>)> done) {
    QNetworkRequest request{QUrl(endpoint_.url(path))};
    request.setTransferTimeout(kRequestTimeoutMs);
    QNetworkReply *reply = network_->get(request);
    guard(reply);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        if (reply->error() != QNetworkReply::NoError || status != 200) {
            done(Error{ErrorCode::Network, status > 0 ? u"Nexa answered %1."_s.arg(status) : reply->errorString()});
            return;
        }
        done(reply->readAll());
    });
}

void RemoteClient::setState(State state, const QString &detail) {
    state_ = state;
    emit stateChanged(state, detail);
}

} // namespace nexa
