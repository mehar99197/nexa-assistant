#pragma once

#include "core/Result.h"

#include <QByteArray>
#include <QDateTime>
#include <QHash>
#include <QHostAddress>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSslConfiguration>
#include <QString>
#include <QStringList>

#include <functional>
#include <memory>
#include <optional>

class QHttpServer;
class QHttpServerRequest;
class QHttpServerResponse;

namespace nexa {

class IRemoteHost;
class PairingManager;
class RemoteSession;
class TlsIdentity;

// The web remote: Nexa's own small web server, so the phone's browser can talk to it.
//
// It serves the page (from Qt resources), trades a pairing code for a token at /api/pair, hands out
// replies read aloud at /speech/<id>.wav, and runs the WebSocket at /ws that carries requests in and
// the conversation out. It listens twice: with TLS on the network at `port`, and without TLS on
// loopback only at `port + 1`, which a phone reaches through `adb reverse` (Nexa sets it up when a
// phone connects) — that link is already encrypted, and it gives the page a secure context with no
// certificate warning.
//
// Every request a phone makes goes through IRemoteHost::submit, exactly like one typed on the PC:
// the same fast path, agent, policy and audit. Each connected phone sees the whole conversation,
// whoever asked.
class RemoteServer : public QObject {
    Q_OBJECT
public:
    static constexpr int kMaxSessions = 8;
    // The port the phone opens through adb reverse: http://localhost:7777, whatever the PC uses.
    static constexpr int kPhonePort = 7777;

    RemoteServer(PairingManager *pairing, IRemoteHost *host, TlsIdentity *tls, QObject *parent = nullptr);
    ~RemoteServer() override;

    // Starts listening, making the certificate first if needed. The result is a sentence saying
    // where — or what failed. Running without TLS (on loopback alone) counts as running.
    void start(int port, std::function<void(Result<QString>)> done);
    void stop();
    bool isRunning() const { return running_; }
    int port() const { return port_; }
    QString lanUrl() const;             // https://<address>:<port>, empty without TLS
    QString localUrl() const;           // what the phone opens through adb reverse
    int localPort() const { return port_ + 1; }   // where adb reverse must point on the PC
    QString fingerprint() const;
    QString problem() const { return problem_; }
    int clientCount() const;

    // The address a phone on the same network would use: Wi-Fi or Ethernet, never a container bridge.
    static QString lanAddress();
    // Pure: same-origin check for a WebSocket upgrade. A browser always sends Origin; a request
    // without one is not from a page, and still has to bring a token.
    static bool sameOrigin(const QString &origin, const QString &host);

public slots:
    // The conversation, from AppController (wired in app/main.cc).
    void onUserMessage(const QString &text);
    void onReplyStarted(const QString &source);
    void onReplyDelta(const QString &delta);
    void onReplyStep(const QString &description, bool ok);
    void onReplyFinished(const QString &reason);
    void onReplyFailed(const QString &message);
    void onBusyChanged(bool busy);
    void onStatus(const QString &message, bool isError);
    // The Nexa core's state and the satellites it lights, as NexaCoreSpec names them on the wire.
    void onCoreChanged(const QString &mode, const QStringList &nodes);

signals:
    void clientsChanged(int count);
    void runningChanged(bool running);

private:
    struct Speech {
        QByteArray wav;
        QDateTime expires;
    };

    bool listen(const std::optional<QSslConfiguration> &tls);
    void setUpRoutes();
    void acceptSockets();
    void broadcast(const QByteArray &message);
    void handleText(RemoteSession *session, const QString &text);
    void handleAudio(RemoteSession *session, const QByteArray &pcm);
    QHttpServerResponse pairRoute(const QHttpServerRequest &request);
    QHttpServerResponse staticFile(const QString &name) const;
    QHttpServerResponse speechRoute(const QString &file);
    bool allowPairingAttempt(const QHostAddress &from);
    bool refuseWhenBusy(RemoteSession *session);
    void pruneSpeeches();

    PairingManager *pairing_;
    IRemoteHost *host_;
    TlsIdentity *tls_;
    std::unique_ptr<QHttpServer> http_;
    QList<RemoteSession *> sessions_;
    QHash<QString, Speech> speeches_;
    QHash<QString, QList<QDateTime>> pairAttempts_;     // by address, the last minute's attempts
    QList<QDateTime> allPairAttempts_;
    QString pendingSource_;                             // the source of the request being submitted
    QString replySource_;
    QString replyText_;
    QString replyStep_;                                 // what the fast path did, for a spoken answer
    QPointer<RemoteSession> speaker_;                   // whose spoken request the reply answers
    QString lanAddress_;
    QString problem_;
    QString mode_{QStringLiteral("idle")};              // the core's state, told to a phone as it connects
    QStringList litNodes_;
    int port_{0};
    bool tlsRunning_{false};
    bool running_{false};
    bool busy_{false};
    quint64 generation_{0};                             // a stop() makes late callbacks stale
};

} // namespace nexa
