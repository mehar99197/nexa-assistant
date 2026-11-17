#pragma once

#include "core/Result.h"
#include "remote/RemoteProtocol.h"
#include "remote/RemoteRoutes.h"

#include <QList>
#include <QSet>
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
    QString via;        // which way this is, in words ("Tailscale", "Cloudflare"); empty for the one paired on
    bool tunnel{false}; // through Cloudflare or ngrok: a certificate the system trusts, never Nexa's own

    bool isValid() const { return !host.isEmpty() && port > 0 && port < 65536; }
    QString url(const QString &path = {}) const;    // https://host:port/path
    QString socketUrl() const;                      // wss://host:port/ws
    // "192.168.0.115:7777", "pc.local", "[::1]:7777", or the invitation the PC's QR code holds,
    // "https://192.168.0.115:7777/#pair=123456". Without a port, Nexa's usual one.
    static std::optional<RemoteEndpoint> parse(const QString &text);
    // A way the PC told about (its welcome's routes).
    static std::optional<RemoteEndpoint> fromRoute(const RemoteRoute &route);
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
    // Other ways to the same PC, tried in turn when one does not answer (away from home: Tailscale, the tunnels). A
    // way of Nexa's own certificate is tried only once its fingerprint is pinned; a tunnel's must be one the system
    // trusts. The token is sent to none before its certificate passed.
    void setAlternates(const QList<RemoteEndpoint> &alternates);
    // The way in use: "" (the one paired on), "Tailscale", "Cloudflare", …
    QString via() const { return current().via; }
    RemoteEndpoint endpointInUse() const { return current(); }
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
    // Upload attachments over POST /api/upload with pinned TLS + Bearer auth (falls back to ask() if empty).
    void uploadAsk(const QString &text, const QStringList &filePaths, std::function<void(Result<void>)> done);
    bool askAloud(const QByteArray &pcm16kMonoInt16);   // one whole utterance
    std::optional<remote::ThemeStyle> theme() const { return theme_; }
    void cancel();
    bool unlock();                                      // Unlock PC (hardcore mode); false when not online
    bool answer(quint64 id, bool allow);                // a question a task waits on

    // GET a path on the server — the spoken reply — under the same trust rules.
    void fetch(const QString &path, std::function<void(Result<QByteArray>)> done);

signals:
    void stateChanged(State state, const QString &detail);
    void welcomed(const QString &device, bool voice, bool busy, const QString &mode);
    // What the PC said of the ways to it (in the welcome, and when they change), and its certificate's fingerprint.
    void routesLearned(const QList<nexa::RemoteRoute> &routes, const QString &fingerprint);
    void connectedVia(const QString &via);
    void received(const nexa::remote::ServerMessage &message);     // everything after the welcome
    void themeChanged(const nexa::remote::ThemeStyle &theme);      // PC palette/design arrived or updated
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
    QList<RemoteEndpoint> ways() const;             // the one paired on, then the alternates
    RemoteEndpoint current() const;
    bool usable(const RemoteEndpoint &way) const;   // its certificate can be checked (or it needs none)
    void tryNext();                                 // this way failed: the next one, or a pause after a round

    RemoteEndpoint endpoint_;
    QList<RemoteEndpoint> alternates_;
    int current_{0};                                // into ways()
    int triedThisRound_{0};
    QSet<QString> warned_;                          // certificates already reported as not the trusted one
    QSet<QString> refused_;                         // ways that showed the wrong certificate: not tried again
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
    std::optional<remote::ThemeStyle> theme_;
};

} // namespace nexa
