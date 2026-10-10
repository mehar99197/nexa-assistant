#include "remote/RemoteServer.h"

#include "core/Logging.h"
#include "remote/IRemoteHost.h"
#include "remote/PairingManager.h"
#include "remote/RemoteProtocol.h"
#include "remote/RemoteSession.h"
#include "remote/TlsIdentity.h"

#include <QFile>
#include <QHttpHeaders>
#include <QHttpServer>
#include <QHttpServerRequest>
#include <QHttpServerResponse>
#include <QHttpServerWebSocketUpgradeResponse>
#include <QJsonDocument>
#include <QJsonObject>
#include <QNetworkInterface>
#include <QRandomGenerator>
#include <QSslServer>
#include <QTcpServer>
#include <QUrl>
#include <QWebSocket>

using namespace Qt::StringLiterals;

namespace nexa {

namespace {

constexpr int kSpeechLifetimeSeconds = 10 * 60;
constexpr qsizetype kMaxSpeeches = 16;
// Pairing attempts per minute: from one address, and from everywhere together.
constexpr qsizetype kPairingAttemptsPerAddress = 5;
constexpr qsizetype kPairingAttemptsTotal = 20;

// The page may load only itself: no scripts, styles or connections anywhere else.
constexpr auto kContentSecurityPolicy =
    "default-src 'self'; script-src 'self'; style-src 'self'; img-src 'self' data:; media-src 'self' blob:; "
    "connect-src 'self'; worker-src 'self'; manifest-src 'self'; base-uri 'none'; form-action 'none'; "
    "frame-ancestors 'none'";

// Everything the server will hand out, and nothing else: no path from the URL reaches the disk.
struct StaticFile {
    const char *name;
    const char *type;
};
constexpr StaticFile kFiles[] = {
    {"index.html", "text/html; charset=utf-8"},
    {"app.js", "text/javascript; charset=utf-8"},
    {"worklet.js", "text/javascript; charset=utf-8"},
    {"sw.js", "text/javascript; charset=utf-8"},
    {"style.css", "text/css; charset=utf-8"},
    {"manifest.json", "application/manifest+json"},
    {"icon.svg", "image/svg+xml"},
};

QString randomId() {
    QByteArray bytes(16, Qt::Uninitialized);
    QRandomGenerator::system()->fillRange(reinterpret_cast<quint32 *>(bytes.data()), 4);
    return QString::fromLatin1(bytes.toHex());
}

QHttpServerResponse noStore(QHttpServerResponse response) {
    QHttpHeaders headers = response.headers();
    headers.append("Cache-Control", "no-store");
    response.setHeaders(std::move(headers));
    return response;
}

bool isVirtual(const QNetworkInterface &interface) {
    static const QStringList prefixes{u"docker"_s, u"br-"_s,   u"veth"_s, u"virbr"_s, u"vmnet"_s, u"vboxnet"_s,
                                      u"lxc"_s,    u"lxd"_s,   u"podman"_s, u"cni"_s, u"flannel"_s};
    for (const QString &prefix : prefixes) {
        if (interface.name().startsWith(prefix)) return true;
    }
    return interface.type() == QNetworkInterface::Virtual;
}

// "write — Opened Firefox · 12 ms" → "Opened Firefox": what the fast path did, said in words.
QString stepSummary(const QString &description) {
    const qsizetype dash = description.indexOf(u" — "_s);
    QString summary = dash >= 0 ? description.mid(dash + 3) : description;
    const qsizetype dot = summary.lastIndexOf(u" · "_s);
    if (dot >= 0) summary.truncate(dot);
    return summary.trimmed();
}

} // namespace

RemoteServer::RemoteServer(PairingManager *pairing, IRemoteHost *host, TlsIdentity *tls, QObject *parent)
    : QObject(parent), pairing_(pairing), host_(host), tls_(tls) {
    // A revoked phone is shut out at once, not at its next connection.
    connect(pairing_, &PairingManager::revoked, this, [this](const QString &id) {
        for (RemoteSession *session : std::as_const(sessions_)) {
            if (session->deviceId() == id) session->close(tr("This phone's pairing was revoked."));
        }
    });
}

RemoteServer::~RemoteServer() {
    stop();
}

void RemoteServer::start(int port, std::function<void(Result<QString>)> done) {
    stop();
    port_ = port;
    problem_.clear();
    lanAddress_ = lanAddress();
    const quint64 generation = generation_;
    const QPointer<RemoteServer> self(this);
    QStringList addresses;
    if (!lanAddress_.isEmpty()) addresses.append(lanAddress_);
    tls_->ensure(addresses, [self, generation, done](Result<QSslConfiguration> tls) {
        if (!self || generation != self->generation_) return;      // stopped or restarted meanwhile
        if (!tls) {
            self->problem_ = tr("No HTTPS on the network (%1), so a phone can only use it through adb.")
                                 .arg(tls.error().message);
        }
        if (!self->listen(tls ? std::optional<QSslConfiguration>(tls.value()) : std::nullopt)) {
            done(Error{ErrorCode::Io, self->problem_});
            return;
        }
        QStringList where;
        if (const QString lan = self->lanUrl(); !lan.isEmpty()) where.append(lan);
        where.append(tr("%1 on a phone connected with adb").arg(self->localUrl()));
        done(tr("The web remote is at %1.").arg(where.join(tr(", and "))));
    });
}

bool RemoteServer::listen(const std::optional<QSslConfiguration> &tls) {
    http_ = std::make_unique<QHttpServer>();
    setUpRoutes();
    QStringList failures;

    tlsRunning_ = false;
    if (tls) {
        auto *server = new QSslServer(http_.get());
        server->setSslConfiguration(*tls);
        if (server->listen(QHostAddress::Any, quint16(port_)) && http_->bind(server)) {
            tlsRunning_ = true;
        } else {
            failures.append(tr("port %1: %2").arg(port_).arg(server->errorString()));
            delete server;
        }
    }
    // Loopback only: the plain-HTTP door is for adb reverse, never for the network.
    auto *local = new QTcpServer(http_.get());
    const bool localRunning = local->listen(QHostAddress::LocalHost, quint16(localPort())) && http_->bind(local);
    if (!localRunning) {
        failures.append(tr("port %1 on this PC: %2").arg(localPort()).arg(local->errorString()));
        delete local;
    }

    if (!tlsRunning_ && !localRunning) {
        problem_ = tr("The web remote could not listen — %1.").arg(failures.join(u"; "_s));
        http_.reset();
        return false;
    }
    if (!failures.isEmpty() && problem_.isEmpty()) problem_ = failures.join(u"; "_s);
    connect(http_.get(), &QAbstractHttpServer::newWebSocketConnection, this, &RemoteServer::acceptSockets);
    running_ = true;
    qCInfo(lcApp).noquote() << "remote: listening —" << (tlsRunning_ ? lanUrl() : u"no TLS"_s) << "and"
                            << u"127.0.0.1:%1"_s.arg(localPort());
    emit runningChanged(true);
    return true;
}

void RemoteServer::stop() {
    ++generation_;
    const QList<RemoteSession *> open = std::exchange(sessions_, {});
    for (RemoteSession *session : open) {
        session->disconnect(this);
        session->close(tr("Nexa's web remote was switched off."));
        session->deleteLater();
    }
    http_.reset();
    speeches_.clear();
    speaker_.clear();
    const bool wasRunning = running_;
    running_ = false;
    tlsRunning_ = false;
    if (wasRunning) {
        qCInfo(lcApp) << "remote: stopped";
        emit runningChanged(false);
        emit clientsChanged(0);
    }
}

QString RemoteServer::lanUrl() const {
    return tlsRunning_ && !lanAddress_.isEmpty() ? u"https://%1:%2"_s.arg(lanAddress_).arg(port_) : QString();
}

QString RemoteServer::localUrl() const {
    return u"http://localhost:%1"_s.arg(kPhonePort);
}

QString RemoteServer::fingerprint() const {
    return tlsRunning_ ? tls_->fingerprint() : QString();
}

int RemoteServer::clientCount() const {
    return int(std::count_if(sessions_.cbegin(), sessions_.cend(),
                             [](const RemoteSession *session) { return session->isAuthenticated(); }));
}

QString RemoteServer::lanAddress() {
    QString fallback;
    const QList<QNetworkInterface> interfaces = QNetworkInterface::allInterfaces();
    for (const QNetworkInterface &interface : interfaces) {
        const QNetworkInterface::InterfaceFlags flags = interface.flags();
        if (!flags.testFlag(QNetworkInterface::IsUp) || !flags.testFlag(QNetworkInterface::IsRunning)
            || flags.testFlag(QNetworkInterface::IsLoopBack) || isVirtual(interface)) {
            continue;
        }
        const QList<QNetworkAddressEntry> entries = interface.addressEntries();
        for (const QNetworkAddressEntry &entry : entries) {
            const QHostAddress address = entry.ip();
            if (address.protocol() != QAbstractSocket::IPv4Protocol || address.isLinkLocal()) continue;
            if (interface.type() == QNetworkInterface::Wifi || interface.type() == QNetworkInterface::Ethernet) {
                return address.toString();
            }
            if (fallback.isEmpty()) fallback = address.toString();
        }
    }
    return fallback;
}

bool RemoteServer::sameOrigin(const QString &origin, const QString &host) {
    if (origin.isEmpty()) return true;
    const QUrl url(origin);
    if (!url.isValid() || url.host().isEmpty() || (url.scheme() != u"https" && url.scheme() != u"http")) return false;

    // The Host header: "name", "name:port", "[v6]" or "[v6]:port".
    QString name = host.trimmed();
    int port = -1;
    if (name.startsWith(u'[')) {
        const qsizetype close = name.indexOf(u']');
        if (close < 0) return false;
        const QString rest = name.mid(close + 1);
        name = name.mid(1, close - 1);
        if (rest.startsWith(u':')) port = rest.mid(1).toInt();
    } else if (const qsizetype colon = name.lastIndexOf(u':'); colon >= 0) {
        port = name.mid(colon + 1).toInt();
        name = name.left(colon);
    }
    const int defaultPort = url.scheme() == u"https" ? 443 : 80;
    if (port < 0) port = defaultPort;
    return url.host().compare(name, Qt::CaseInsensitive) == 0 && url.port(defaultPort) == port;
}

void RemoteServer::setUpRoutes() {
    QHttpServer *http = http_.get();
    http->route(u"/"_s, QHttpServerRequest::Method::Get, [this] { return staticFile(u"index.html"_s); });
    http->route(u"/api/pair"_s, QHttpServerRequest::Method::Post,
                [this](const QHttpServerRequest &request) { return pairRoute(request); });
    http->route(u"/speech/<arg>"_s, QHttpServerRequest::Method::Get,
                [this](const QString &file) { return speechRoute(file); });
    http->route(u"/<arg>"_s, QHttpServerRequest::Method::Get, [this](const QString &name) { return staticFile(name); });

    http->addAfterRequestHandler(this, [](const QHttpServerRequest &, QHttpServerResponse &response) {
        QHttpHeaders headers = response.headers();
        headers.append("Content-Security-Policy", kContentSecurityPolicy);
        headers.append("X-Content-Type-Options", "nosniff");
        headers.append("Referrer-Policy", "no-referrer");
        headers.append("X-Frame-Options", "DENY");
        if (!headers.contains("Cache-Control")) headers.append("Cache-Control", "no-cache");
        response.setHeaders(std::move(headers));
    });

    // Only /ws upgrades, and only from the page this server served: another site open in the phone's
    // browser cannot talk to Nexa through it.
    http->addWebSocketUpgradeVerifier(this, [](const QHttpServerRequest &request) {
        if (request.url().path() != u"/ws") return QHttpServerWebSocketUpgradeResponse::passToNext();
        const QString origin = QString::fromUtf8(request.headers().value("Origin"));
        const QString host = QString::fromUtf8(request.headers().value("Host"));
        if (!sameOrigin(origin, host)) {
            qCInfo(lcApp).noquote() << "remote: refused a WebSocket from origin" << origin;
            return QHttpServerWebSocketUpgradeResponse::deny(403, "Wrong origin");
        }
        return QHttpServerWebSocketUpgradeResponse::accept();
    });
}

QHttpServerResponse RemoteServer::staticFile(const QString &name) const {
    for (const StaticFile &file : kFiles) {
        if (name != QLatin1StringView(file.name)) continue;
        QFile resource(u":/nexa/web/"_s + name);
        if (!resource.open(QIODevice::ReadOnly)) break;
        return QHttpServerResponse(QByteArray(file.type), resource.readAll());
    }
    return QHttpServerResponse(QHttpServerResponse::StatusCode::NotFound);
}

QHttpServerResponse RemoteServer::pairRoute(const QHttpServerRequest &request) {
    if (!allowPairingAttempt(request.remoteAddress())) {
        return noStore(QHttpServerResponse(QJsonObject{{u"error"_s, tr("Too many attempts. Wait a minute.")}},
                                           QHttpServerResponse::StatusCode::TooManyRequests));
    }
    const QJsonObject body = QJsonDocument::fromJson(request.body()).object();
    const QString code = body.value(u"code"_s).toString();
    if (code.isEmpty()) {
        return noStore(QHttpServerResponse(QJsonObject{{u"error"_s, tr("Enter the pairing code.")}},
                                           QHttpServerResponse::StatusCode::BadRequest));
    }
    const Result<PairingManager::Paired> paired = pairing_->pair(code, body.value(u"name"_s).toString());
    if (!paired) {
        qCInfo(lcApp).noquote() << "remote: a pairing attempt from" << request.remoteAddress().toString() << "failed";
        return noStore(QHttpServerResponse(QJsonObject{{u"error"_s, paired.error().message}},
                                           QHttpServerResponse::StatusCode::Forbidden));
    }
    return noStore(QHttpServerResponse(
        QJsonObject{{u"token"_s, paired.value().token}, {u"device"_s, paired.value().device.name}}));
}

bool RemoteServer::allowPairingAttempt(const QHostAddress &from) {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    const auto lastMinute = [&now](QList<QDateTime> &times) {
        times.removeIf([&now](const QDateTime &at) { return at.secsTo(now) >= 60; });
    };
    lastMinute(allPairAttempts_);
    pairAttempts_.removeIf([&](QHash<QString, QList<QDateTime>>::iterator it) {
        lastMinute(it.value());
        return it.value().isEmpty();
    });
    QList<QDateTime> &mine = pairAttempts_[from.toString()];
    if (mine.size() >= kPairingAttemptsPerAddress || allPairAttempts_.size() >= kPairingAttemptsTotal) return false;
    mine.append(now);
    allPairAttempts_.append(now);
    return true;
}

QHttpServerResponse RemoteServer::speechRoute(const QString &file) {
    pruneSpeeches();
    const QString id = file.endsWith(u".wav"_s) ? file.chopped(4) : file;
    const auto found = speeches_.constFind(id);
    if (found == speeches_.cend()) return QHttpServerResponse(QHttpServerResponse::StatusCode::NotFound);
    return noStore(QHttpServerResponse(QByteArrayLiteral("audio/wav"), found->wav));
}

void RemoteServer::pruneSpeeches() {
    const QDateTime now = QDateTime::currentDateTimeUtc();
    speeches_.removeIf([&now](QHash<QString, Speech>::iterator it) { return it.value().expires <= now; });
    // The oldest go first when too many pile up.
    while (speeches_.size() >= kMaxSpeeches) {
        auto oldest = speeches_.begin();
        for (auto it = speeches_.begin(); it != speeches_.end(); ++it) {
            if (it.value().expires < oldest.value().expires) oldest = it;
        }
        speeches_.erase(oldest);
    }
}

void RemoteServer::acceptSockets() {
    while (http_ && http_->hasPendingWebSocketConnections()) {
        std::unique_ptr<QWebSocket> socket = http_->nextPendingWebSocketConnection();
        if (!socket) continue;
        if (sessions_.size() >= kMaxSessions) {
            QWebSocket *refused = socket.release();
            refused->abort();
            refused->deleteLater();
            continue;
        }
        auto *session = new RemoteSession(std::move(socket), pairing_, this);
        sessions_.append(session);
        connect(session, &RemoteSession::authenticated, this, [this, session] {
            session->send(remote::welcome(session->deviceName(), host_->canTranscribe(), busy_ || host_->isBusy()));
            emit clientsChanged(clientCount());
        });
        connect(session, &RemoteSession::textRequest, this,
                [this, session](const QString &text) { handleText(session, text); });
        connect(session, &RemoteSession::audioRequest, this,
                [this, session](const QByteArray &pcm) { handleAudio(session, pcm); });
        connect(session, &RemoteSession::cancelRequested, this, [this] { host_->cancel(); });
        connect(session, &RemoteSession::closed, this, [this, session] {
            sessions_.removeOne(session);
            session->deleteLater();
            emit clientsChanged(clientCount());
        });
    }
}

void RemoteServer::broadcast(const QByteArray &message) {
    for (RemoteSession *session : std::as_const(sessions_)) {
        if (session->isAuthenticated()) session->send(message);
    }
}

bool RemoteServer::refuseWhenBusy(RemoteSession *session) {
    if (!host_->isBusy()) return false;
    session->send(remote::error(u"busy"_s, tr("Nexa is busy with another request. Wait for it, or cancel it.")));
    return true;
}

void RemoteServer::handleText(RemoteSession *session, const QString &text) {
    if (refuseWhenBusy(session)) return;
    // submit() reports the request back synchronously (onUserMessage), so the source is known there.
    pendingSource_ = u"phone"_s;
    host_->submit(text, u"phone"_s);
    pendingSource_.clear();
}

void RemoteServer::handleAudio(RemoteSession *session, const QByteArray &pcm) {
    if (!host_->canTranscribe()) {
        session->send(remote::error(u"no-voice"_s, tr("Speech recognition is not set up on the PC (Settings → Voice).")));
        return;
    }
    if (refuseWhenBusy(session)) return;
    const QPointer<RemoteServer> self(this);
    const QPointer<RemoteSession> from(session);
    const quint64 generation = generation_;
    host_->transcribe(pcm, [self, from, generation](Result<QString> text) {
        if (!self || !from || generation != self->generation_) return;
        if (!text) {
            from->send(remote::error(u"invalid"_s, tr("The recording could not be understood: %1").arg(text.error().message)));
            return;
        }
        const QString words = text.value().trimmed();
        if (words.isEmpty()) {
            from->send(remote::error(u"invalid"_s, tr("Nothing was heard in that recording.")));
            return;
        }
        from->send(remote::transcript(words));
        if (self->refuseWhenBusy(from)) return;
        self->speaker_ = from;
        self->pendingSource_ = u"phone-voice"_s;
        self->host_->submit(words, u"phone-voice"_s);
        self->pendingSource_.clear();
    });
}

void RemoteServer::onUserMessage(const QString &text) {
    broadcast(remote::accepted(text, pendingSource_.isEmpty() ? u"pc"_s : pendingSource_));
}

void RemoteServer::onReplyStarted(const QString &source) {
    replySource_ = source;
    replyText_.clear();
    replyStep_.clear();
    if (source != u"phone-voice") speaker_.clear();
}

void RemoteServer::onReplyDelta(const QString &delta) {
    replyText_ += delta;
    broadcast(remote::delta(delta));
}

void RemoteServer::onReplyStep(const QString &description, bool ok) {
    if (ok) replyStep_ = stepSummary(description);
    broadcast(remote::step(description, ok));
}

void RemoteServer::onReplyFinished(const QString &reason) {
    broadcast(remote::finished(reason));
    // A spoken request from the phone is answered aloud on the phone: the reply, or — when the
    // fast path answered with an action and no words — what it did.
    const QPointer<RemoteSession> to = std::exchange(speaker_, nullptr);
    const QString text = replyText_.trimmed().isEmpty() ? replyStep_ : replyText_.trimmed();
    if (replySource_ != u"phone-voice" || !to || text.isEmpty() || reason != u"stop") return;
    const QPointer<RemoteServer> self(this);
    host_->renderSpeech(text, [self, to](Result<QByteArray> wav) {
        if (!self || !to) return;
        if (!wav) {
            to->send(remote::error(u"no-voice"_s, tr("The reply could not be read aloud: %1").arg(wav.error().message)));
            return;
        }
        self->pruneSpeeches();
        const QString id = randomId();
        self->speeches_.insert(id, Speech{wav.value(), QDateTime::currentDateTimeUtc().addSecs(kSpeechLifetimeSeconds)});
        to->send(remote::speech(u"/speech/%1.wav"_s.arg(id)));
    });
}

void RemoteServer::onReplyFailed(const QString &message) {
    speaker_.clear();
    broadcast(remote::failed(message));
}

void RemoteServer::onBusyChanged(bool busy) {
    busy_ = busy;
    broadcast(remote::busy(busy));
}

void RemoteServer::onStatus(const QString &message, bool isError) {
    if (isError) broadcast(remote::error(u"status"_s, message));
}

} // namespace nexa
