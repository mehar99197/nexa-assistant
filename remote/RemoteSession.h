#pragma once

#include <QByteArray>
#include <QElapsedTimer>
#include <QList>
#include <QObject>
#include <QString>
#include <QTimer>

#include <memory>

class QWebSocket;

namespace nexa {

class PairingManager;

// Adapter between one browser's WebSocket and the rest of the server: it must say hello with its
// token within a few seconds or it is dropped, and only then do its messages become requests.
// Rate-limited, so a page gone wrong — or a stolen tab — cannot flood Nexa with work.
class RemoteSession : public QObject {
    Q_OBJECT
public:
    static constexpr int kHelloTimeoutMs = 10'000;
    static constexpr int kMaxRequestsPerMinute = 12;

    RemoteSession(std::unique_ptr<QWebSocket> socket, PairingManager *pairing, QObject *parent = nullptr);
    ~RemoteSession() override;

    bool isAuthenticated() const { return !deviceId_.isEmpty(); }
    QString deviceId() const { return deviceId_; }
    QString deviceName() const { return deviceName_; }

    void send(const QByteArray &message);
    void close(const QString &reason);

signals:
    void authenticated();
    void textRequest(const QString &text);
    void audioRequest(const QByteArray &pcm16k);
    void cancelRequested();
    void unlockRequested();
    void answered(quint64 id, bool allow);
    void closed();

private:
    void onText(const QString &frame);
    void onBinary(const QByteArray &frame);
    bool allowRequest();

    std::unique_ptr<QWebSocket> socket_;
    PairingManager *pairing_;
    QTimer helloTimeout_;
    QString deviceId_;
    QString deviceName_;
    QElapsedTimer clock_;
    QList<qint64> recent_;          // when the requests of the last minute arrived
    bool closing_{false};
};

} // namespace nexa
