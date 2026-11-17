#include "remote/RemoteClient.h"

#include "core/Logging.h"

#include <QCryptographicHash>
#include <QFile>
#include <QFileInfo>
#include <QHostAddress>
#include <QJsonArray>
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

// Qt for Android brings no TLS of its own: an APK without OpenSSL inside it (android/CMakeLists.txt,
// scripts/download-android-openssl.sh) answers every https:// and wss:// address with Qt's bare
// "TLS initialization failed". Said in words instead, before anything is tried.
std::optional<Error> missingTls() {
    if (QSslSocket::supportsSsl()) return std::nullopt;
    return Error{ErrorCode::NotSupported,
                 u"this app was built without TLS (OpenSSL is missing from it), so it can reach Nexa only at localhost through adb"_s};
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
    // "pc.local" means Nexa's port; a web address without one means the web's (a tunnel's https://nexa.example.com).
    const bool webAddress = trimmed.contains(u"://"_s);
    endpoint.port = url.port(webAddress ? (url.scheme() == u"http" ? 80 : 443) : kDefaultPort);
    endpoint.tls = !isLoopback(endpoint.host);
    static const QRegularExpression invitation(u"pair=(\\d{6})"_s);
    endpoint.code = invitation.match(url.fragment()).captured(1);
    return endpoint;
}

std::optional<RemoteEndpoint> RemoteEndpoint::fromRoute(const RemoteRoute &route) {
    std::optional<RemoteEndpoint> endpoint = parse(route.url);
    if (!endpoint) return std::nullopt;
    endpoint->code.clear();
    endpoint->via = routeName(route.kind);
    endpoint->tunnel = route.isTunnel();
    endpoint->tls = route.kind != RemoteRoute::Kind::Adb;
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
    if (const std::optional<Error> missing = missingTls()) {
        done(*missing);
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
    request.setRawHeader("ngrok-skip-browser-warning", "1");
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

void RemoteClient::setAlternates(const QList<RemoteEndpoint> &alternates) {
    alternates_.clear();
    for (const RemoteEndpoint &way : alternates) {
        if (!way.isValid()) continue;
        const bool known = way.url() == endpoint_.url()
                           || std::any_of(alternates_.cbegin(), alternates_.cend(),
                                          [&way](const RemoteEndpoint &kept) { return kept.url() == way.url(); });
        if (!known) alternates_.append(way);
    }
    if (current_ >= int(ways().size())) current_ = 0;
}

QList<RemoteEndpoint> RemoteClient::ways() const {
    QList<RemoteEndpoint> all{endpoint_};
    all.append(alternates_);
    return all;
}

RemoteEndpoint RemoteClient::current() const {
    const QList<RemoteEndpoint> all = ways();
    return all.value(std::clamp(current_, 0, int(all.size()) - 1));
}

bool RemoteClient::usable(const RemoteEndpoint &way) const {
    if (!way.isValid() || refused_.contains(way.url())) return false;
    if (!way.tls) return true;
    if (missingTls()) return false;
    return way.tunnel || !pinned_.isEmpty();
}

void RemoteClient::tryNext() {
    const int count = int(ways().size());
    ++triedThisRound_;
    if (triedThisRound_ < count) {
        current_ = (current_ + 1) % count;
        retry_.start(300);      // the next way at once — a phone away from home should not wait for each
        setState(State::Connecting, u"trying another way…"_s);
        return;
    }
    // Every way was tried: a pause, then the round again from the first.
    triedThisRound_ = 0;
    current_ = 0;
    const int delay = std::min(kMaxRetryDelayMs, 1000 * (1 << std::min(attempts_, 4)));
    ++attempts_;
    retry_.start(delay);
    setState(State::Waiting, u"offline — trying again in %1 s"_s.arg(delay / 1000));
}

void RemoteClient::connectToNexa() {
    retry_.stop();
    rejected_ = false;
    attempts_ = 0;
    triedThisRound_ = 0;
    refused_.clear();
    if (socket_->state() != QAbstractSocket::UnconnectedState) {
        // A fresh start, not a retry: whatever was there goes, quietly.
        wanted_ = false;
        socket_->abort();
    }
    wanted_ = true;
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
    // A way whose certificate could not be checked is passed over; with none left, it says why.
    const QList<RemoteEndpoint> all = ways();
    if (std::none_of(all.cbegin(), all.cend(), [this](const RemoteEndpoint &way) { return usable(way); })) {
        wanted_ = false;
        const std::optional<Error> missing = missingTls();
        setState(State::Offline, missing ? missing->message : u"Nexa's certificate has not been trusted yet."_s);
        return;
    }
    while (!usable(current())) {
        current_ = (current_ + 1) % int(all.size());
    }
    rejected_ = false;
    welcomed_ = false;
    setState(State::Connecting, u"connecting…"_s);
    QNetworkRequest request{QUrl(current().socketUrl())};
    // ngrok's free plan shows a browser a warning page first; an app says it is not one.
    request.setRawHeader("ngrok-skip-browser-warning", "1");
    socket_->open(request);
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
    if (retry_.isActive()) return;
    if (rejected_) refused_.insert(current().url());
    const QList<RemoteEndpoint> all = ways();
    if (std::none_of(all.cbegin(), all.cend(), [this](const RemoteEndpoint &way) { return usable(way); })) {
        // Every way left showed a certificate that is not the trusted one: trying again would not change that.
        wanted_ = false;
        setState(State::Offline, u"Nexa's certificate is not the one that was trusted."_s);
        return;
    }
    // A way that did not answer, or showed the wrong certificate, is left for the next; the token was never sent.
    tryNext();
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
        triedThisRound_ = 0;
        ping_.start();
        setState(State::Online, message->device);
        emit welcomed(message->device, message->voice, message->busy, message->mode);
        emit connectedVia(current().via);
        emit routesLearned(message->routes, message->fingerprint);
        if (message->theme) {
            theme_ = message->theme;
            emit themeChanged(*theme_);
        }
        return;
    case remote::ServerMessage::Type::Theme:
        if (message->theme) {
            theme_ = message->theme;
            emit themeChanged(*theme_);
        }
        return;
    case remote::ServerMessage::Type::Routes:
        emit routesLearned(message->routes, {});
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
    // Said once per certificate: the next way is tried, and the round comes back to this one again and again.
    if (!warned_.contains(seen)) {
        warned_.insert(seen);
        emit certificateRejected(seen);
    }
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
    // Through a tunnel the certificate is the tunnel's, and the system must trust it: Nexa's own there is not right.
    if (current().tunnel) return false;
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

void RemoteClient::uploadAsk(const QString &text, const QStringList &filePaths, std::function<void(Result<void>)> done) {
    if (filePaths.isEmpty()) {
        if (ask(text)) {
            if (done) done(Result<void>{});
        } else if (done) {
            done(Error{ErrorCode::Network, u"Not connected to Nexa."_s});
        }
        return;
    }
    if (!welcomed_ || token_.isEmpty()) {
        if (done) done(Error{ErrorCode::Network, u"Not connected to Nexa."_s});
        return;
    }
    if (filePaths.size() > 8) {
        if (done) done(Error{ErrorCode::InvalidArgument, u"At most 8 files may be attached."_s});
        return;
    }
    QJsonArray filesJson;
    for (const QString &path : filePaths) {
        QFile file(path);
        if (!file.open(QIODevice::ReadOnly)) {
            if (done) done(Error{ErrorCode::Io, u"Cannot open %1: %2"_s.arg(path, file.errorString())});
            return;
        }
        constexpr qint64 kMaxFileBytes = 8 * 1024 * 1024;
        if (file.size() > kMaxFileBytes) {
            if (done) done(Error{ErrorCode::InvalidArgument, u"%1 exceeds the 8 MB limit."_s.arg(QFileInfo(path).fileName())});
            return;
        }
        const QByteArray bytes = file.readAll();
        QString name = QFileInfo(path).fileName();
        if (name.isEmpty()) name = QUrl(path).fileName();
        if (name.isEmpty()) name = u"attachment.bin"_s;
        filesJson.append(QJsonObject{
            {u"name"_s, name},
            {u"data"_s, QString::fromLatin1(bytes.toBase64())},
        });
    }
    QNetworkRequest request{QUrl(current().url(u"/api/upload"_s))};
    request.setRawHeader("ngrok-skip-browser-warning", "1");
    request.setRawHeader("Authorization", "Bearer " + token_.toUtf8());
    request.setHeader(QNetworkRequest::ContentTypeHeader, u"application/json"_s);
    request.setTransferTimeout(kRequestTimeoutMs);
    const QByteArray body = QJsonDocument(QJsonObject{
        {u"text"_s, text.trimmed()},
        {u"files"_s, filesJson},
    }).toJson(QJsonDocument::Compact);
    QNetworkReply *reply = network_->post(request, body);
    guard(reply);
    connect(reply, &QNetworkReply::finished, this, [reply, done = std::move(done)] {
        reply->deleteLater();
        const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray raw = reply->readAll();
        if (reply->error() == QNetworkReply::NoError && status >= 200 && status < 300) {
            if (done) done(Result<void>{});
            return;
        }
        const QJsonObject obj = QJsonDocument::fromJson(raw).object();
        QString msg = obj.value(u"error"_s).toString();
        if (msg.isEmpty()) msg = status > 0 ? u"Upload failed (%1)."_s.arg(status) : reply->errorString();
        if (done) done(Error{ErrorCode::Network, msg});
    });
}

bool RemoteClient::askAloud(const QByteArray &pcm16kMonoInt16) {
    if (!welcomed_ || pcm16kMonoInt16.isEmpty() || pcm16kMonoInt16.size() > remote::kMaxAudioFrame) return false;
    socket_->sendBinaryMessage(pcm16kMonoInt16);
    return true;
}

void RemoteClient::cancel() {
    if (welcomed_) socket_->sendTextMessage(QString::fromUtf8(remote::cancel()));
}

bool RemoteClient::unlock() {
    if (!welcomed_) return false;
    socket_->sendTextMessage(QString::fromUtf8(remote::unlock()));
    return true;
}

bool RemoteClient::answer(quint64 id, bool allow) {
    if (!welcomed_ || id == 0) return false;
    socket_->sendTextMessage(QString::fromUtf8(remote::answer(id, allow)));
    return true;
}

void RemoteClient::fetch(const QString &path, std::function<void(Result<QByteArray>)> done) {
    QNetworkRequest request{QUrl(current().url(path))};
    request.setRawHeader("ngrok-skip-browser-warning", "1");
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
