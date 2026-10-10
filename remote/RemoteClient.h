#pragma once

#include "core/Result.h"
#include "remote/RemoteProtocol.h"

#include <QList>
#include <QObject>
#include <QSslCertificate>
#include <QString>
#include <QTimer>

#include <functional>
#include <memory>
#include <optional>

class QNetworkAccessManager;
class QNetworkReply;
class QSslError;
class QWebSocket;

namespace nexa {

// Where Nexa is, as a phone reaches it. Over the network it is HTTPS with Nexa's own certificate;
// through `adb reverse` it is plain HTTP on the phone's loopback, so a loopback host means no TLS.
struct RemoteEndpoint {
    static constexpr int kDefaultPort = 7777;

    QString host;
    int port{kDefaultPort};
    bool tls{true};
    QString code;       // the pairing code a pasted invitation carried, else empty

    bool isValid() const { return !host.isEmpty() && port > 0 && port < 65536; }
    QString url(const QString &path = {}) const;    // https://host:port/path
    QString socketUrl() const;                      // wss://host:port/ws
    // "192.168.0.115:7777", "pc.local", "[::1]:7777", or the invitation the PC's QR code holds,
    // "https://192.168.0.115:7777/#pair=123456". Without a port, Nexa's usual one.
    static std::optional<RemoteEndpoint> parse(const QString &text);
};

// The fingerprint Settings → Remote shows on the PC: SHA-256 of the certificate, colon-separated hex.
QString fingerprintOf(const QSslCertificate &certificate);

// The phone's end of the web remote, for Nexa's own app (Facade over QWebSocket and
// QNetworkAccessManager; the connection is a small state machine). It pairs with a code, keeps a
// WebSocket to Nexa open with the token — trying again with backoff when it drops — sends typed
// and spoken requests, and relays the conversation. Nexa's certificate is trusted on first use,
// after the user has compared its fingerprint with the PC's, and only that certificate afterwards:
// a different one is refused before anything is sent.
class RemoteClient : public QObject {
    Q_OBJECT
public:
    enum class State { Offline, Connecting, Online, Waiting };      // Waiting: for the next attempt
    Q_ENUM(State)

    struct Certificate {
        QString fingerprint;
        bool trusted{false};    // by the system already (a real CA), so there is nothing to compare
    };

    static constexpr int kPingIntervalMs = 25'000;
    static constexpr int kMaxRetryDelayMs = 15'000;

    explicit RemoteClient(QObject *parent = nullptr);
    ~RemoteClient() override;

    void setEndpoint(const RemoteEndpoint &endpoint);
    const RemoteEndpoint &endpoint() const { return endpoint_; }
    // The fingerprint the user accepted. Without one, only a plain endpoint can be reached.
    void setPinnedFingerprint(const QString &fingerprint) { pinned_ = fingerprint; }
    QString pinnedFingerprint() const { return pinned_; }
    void setToken(const QString &token) { token_ = token; }
    QString token() const { return token_; }
    bool hasToken() const { return !token_.isEmpty(); }

    // The certificate Nexa presents at the endpoint, for the user to compare before it is trusted.
    // Nothing is sent. A plain endpoint reports an empty, trusted certificate.
    void probe(std::function<void(Result<Certificate>)> done);
    // POST /api/pair: the six-digit code shown on the PC for a token, which is kept.
    void pair(const QString &code, const QString &deviceName, std::function<void(Result<QString>)> done);

    void connectToNexa();
    void disconnectFromNexa();
    State state() const { return state_; }
    bool isOnline() const { return state_ == State::Online; }

    bool ask(const QString &text);                      // false when not online
    bool askAloud(const QByteArray &pcm16kMonoInt16);   // one whole utterance
    void cancel();

    // GET a path on the server — the spoken reply — under the same trust rules.
    void fetch(const QString &path, std::function<void(Result<QByteArray>)> done);

signals:
    void stateChanged(State state, const QString &detail);
    void welcomed(const QString &device, bool voice, bool busy, const QString &mode);
    void received(const nexa::remote::ServerMessage &message);     // everything after the welcome
    void unauthorized(const QString &reason);                       // the token was refused: pair again
    void certificateRejected(const QString &fingerprint);           // not the pinned one; nothing was sent

private:
    void open();
    void onConnected();
    void onSocketStateChanged();
    void onTextMessage(const QString &text);
    void onSocketSslErrors(const QList<QSslError> &errors);
    void guard(QNetworkReply *reply);
    bool trusts(const QList<QSslError> &errors, const QSslCertificate &peer, QString *seen) const;
    void setState(State state, const QString &detail = {});

    RemoteEndpoint endpoint_;
    QString pinned_;
    QString token_;
    std::unique_ptr<QWebSocket> socket_;
    std::unique_ptr<QNetworkAccessManager> network_;
    QTimer ping_;
    QTimer retry_;
    State state_{State::Offline};
    bool wanted_{false};
    bool welcomed_{false};
    bool rejected_{false};  // the certificate: no point trying again
    int attempts_{0};
};

} // namespace nexa
